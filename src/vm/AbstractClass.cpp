// Copyright 2017-2018 zzu_softboy <zzu_softboy@163.com>
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
// NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Created by zzu_softboy on 2017/08/01.

#include <iostream>
#include <cstring>
#include "zapi/vm/Interfaces.h"
#include "zapi/vm/AbstractClass.h"
#include "zapi/vm/internal/AbstractClassPrivate.h"
#include "zapi/vm/ObjectBinder.h"
#include "zapi/vm/AbstractMember.h"
#include "zapi/vm/StringMember.h"
#include "zapi/vm/BoolMember.h"
#include "zapi/vm/FloatMember.h"
#include "zapi/vm/NumericMember.h"
#include "zapi/vm/NullMember.h"
#include "zapi/ds/Variant.h"
#include "zapi/lang/Method.h"
#include "zapi/lang/StdClass.h"
#include "zapi/lang/Constant.h"
#include "zapi/lang/Property.h"
#include "zapi/lang/Method.h"
#include "zapi/lang/Interface.h"
#include "zapi/lang/Parameters.h"
#include "zapi/kernel/NotImplemented.h"
#include "zapi/kernel/OrigException.h"
#include "zapi/kernel/AbstractIterator.h"
#include "zapi/utils/PhpFuncs.h"

namespace zapi
{
namespace vm
{

using zapi::lang::Constant;
using zapi::lang::Variant;
using zapi::lang::Property;
using zapi::lang::Method;
using zapi::lang::Interface;
using zapi::lang::Parameters;
using zapi::lang::StdClass;
using zapi::vm::ObjectBinder;
using zapi::vm::Countable;
using zapi::vm::Traversable;
using zapi::vm::Serializable;
using zapi::vm::ArrayAccess;
using zapi::kernel::NotImplemented;
using zapi::kernel::Exception;
using zapi::kernel::AbstractIterator;
using zapi::kernel::process_exception;

namespace internal
{
namespace
{
AbstractClassPrivate *retrieve_acp_ptr_from_cls_entry(zend_class_entry *entry)
{
   // we hide the pointer in entry->info.user.doc_comment
   // the format is \0 + pointer_address
   // if entry->info.user.doc_comment length > 0 or nullptr no pointer hide in it
   while (entry->parent && (nullptr == entry->info.user.doc_comment ||
                            ZSTR_LEN(entry->info.user.doc_comment) > 0))
   {
      // we find the pointer in parent classs
      entry = entry->parent;
   }
   const char *comment = ZSTR_VAL(entry->info.user.doc_comment);
   // here we retrieve the second byte, it have the pointer infomation
   return *reinterpret_cast<AbstractClassPrivate **>(const_cast<char *>(comment + 1));
}

void acp_ptr_deleter(zend_string *ptr)
{
   zend_string_release(ptr);
}
} // anonymous namespace

struct CallContext
{
   zend_internal_function m_func;
   AbstractClassPrivate *m_selfPtr;
};

class ScopedFree
{
public:
   ScopedFree(void *data)
      : m_data(data)
   {}
   ~ScopedFree()
   {
      efree(m_data);
   }
private:
   void *m_data;
};

AbstractClassPrivate::AbstractClassPrivate(const char *className, lang::ClassType type)
   : m_name(className),
     m_type(type),
     m_self(nullptr, acp_ptr_deleter)
{}

zend_class_entry *AbstractClassPrivate::initialize(AbstractClass *cls, const std::string &ns, int moduleNumber)
{
   m_apiPtr = cls;
   zend_class_entry entry;
   if (ns.size() > 0 && ns != "\\") {
      m_name = ns + "\\" + m_name;
   }
   // initialize the class entry
   INIT_CLASS_ENTRY_EX(entry, m_name.c_str(), m_name.size(), getMethodEntries().get());
   entry.create_object = &AbstractClassPrivate::createObject;
   entry.get_static_method = &AbstractClassPrivate::getStaticMethod;
   // check if traversable
   // check if serializable
   if (m_parent) {
      if (m_parent->m_implPtr->m_classEntry) {
         m_classEntry = zend_register_internal_class_ex(&entry, m_parent->m_implPtr->m_classEntry);
      } else {
         std::cerr << "Derived class " << m_name << " is initialized before base class " << m_parent->m_implPtr->m_name
                   << ": base class is ignored" << std::endl;
         // ignore base class
         m_classEntry = zend_register_internal_class(&entry);
      }
   } else {
      m_classEntry = zend_register_internal_class(&entry);
   }
   // register the interfaces of the class
   for (std::shared_ptr<AbstractClass> &interface : m_interfaces) {
      if (interface->m_implPtr->m_classEntry) {
         zend_class_implements(m_classEntry, 1, interface->m_implPtr->m_classEntry);
      } else {
         // interface that want to implement is not initialized
         std::cerr << "Derived class " << m_name << " is initialized before base class "
                   << interface->m_implPtr->m_name << ": interface is ignored"
                   << std::endl;
      }
   }
   m_classEntry->ce_flags = static_cast<uint32_t>(m_type);
   
   for (std::shared_ptr<AbstractMember> &member : m_members) {
      member->initialize(m_classEntry);
   }
   // save AbstractClassPrivate instance pointer into the info.user.doc_comment of zend_class_entry
   // we need save the address of this pointer
   AbstractClassPrivate *selfPtr = this;
   m_self.reset(zend_string_alloc(sizeof(this), 1));
   // make the string look like empty
   ZSTR_VAL(m_self)[0] = '\0';
   ZSTR_LEN(m_self) = 0;
   std::memcpy(ZSTR_VAL(m_self.get()) + 1, &selfPtr, sizeof(selfPtr));
   // save into the doc_comment
   m_classEntry->info.user.doc_comment = m_self.get();
   return m_classEntry;
}

std::unique_ptr<zend_function_entry[]>& AbstractClassPrivate::getMethodEntries()
{
   if (m_methodEntries) {
      return m_methodEntries;
   }
   m_methodEntries.reset(new zend_function_entry[m_methods.size() + 1]);
   size_t i = 0;
   for (std::shared_ptr<Method> &method : m_methods) {
      zend_function_entry *entry = &m_methodEntries[i++];
      method->initialize(entry, m_name.c_str());
   }
   zend_function_entry *last = &m_methodEntries[i];
   memset(last, 0, sizeof(*last));
   return m_methodEntries;
}

zend_object_handlers *AbstractClassPrivate::getObjectHandlers()
{
   if (m_intialized) {
      return &m_handlers;
   }
   memcpy(&m_handlers, &std_object_handlers, sizeof(zend_object_handlers));
   if (!m_apiPtr->clonable()) {
      m_handlers.clone_obj = nullptr;
   } else {
      m_handlers.clone_obj = &AbstractClassPrivate::cloneObject;
   }
   // function for array access interface
   m_handlers.count_elements = &AbstractClassPrivate::countElements;
   m_handlers.write_dimension = &AbstractClassPrivate::writeDimension;
   m_handlers.read_dimension = &AbstractClassPrivate::readDimension;
   m_handlers.has_dimension = &AbstractClassPrivate::hasDimension;
   m_handlers.unset_dimension = &AbstractClassPrivate::unsetDimension;
   
   // functions for magic properties handlers __get, __set, __isset and __unset
   m_handlers.write_property = &AbstractClassPrivate::writeProperty;
   m_handlers.read_property = &AbstractClassPrivate::readProperty;
   m_handlers.has_property = &AbstractClassPrivate::hasProperty;
   m_handlers.unset_property = &AbstractClassPrivate::unsetProperty;
   
   // functions for method is called
   m_handlers.get_method = &AbstractClassPrivate::getMethod;
   m_handlers.get_closure = &AbstractClassPrivate::getClosure;
   
   // functions for object destruct
   m_handlers.dtor_obj = &AbstractClassPrivate::destructObject;
   m_handlers.free_obj = &AbstractClassPrivate::freeObject;
   
   // functions for type cast
   m_handlers.cast_object = &AbstractClassPrivate::cast;
   m_handlers.compare_objects = &AbstractClassPrivate::compare;
   // we set offset here zend engine will free ObjectBinder::m_container
   // resource automatic
   // this offset is very important if you set this not right, memory will leak
   m_handlers.offset = ObjectBinder::calculateZendObjectOffset();
   m_intialized = true;
   return &m_handlers;
}

zend_object *AbstractClassPrivate::createObject(zend_class_entry *entry)
{
   // here we lose everything from AbstractClass object
   // of course we are in static method of AbstractClass
   // but we need get pointer to it, we need some meta info in it and we must
   // instantiate native c++ class associated with the meta class
   AbstractClassPrivate *abstractClsPrivatePtr = retrieve_acp_ptr_from_cls_entry(entry);
   // note: here we use StdClass type to store Derived class
   StdClass *nativeObject = abstractClsPrivatePtr->m_apiPtr->construct();
   if (!nativeObject) {
      // report error on failure, because this function is called directly from the
      // Zend engine, we can call zend_error() here (which does a longjmp() back to
      // the Zend engine)
      zend_error(E_ERROR, "Unable to instantiate %s", entry->name->val);
   }
   // here we assocaited a native object with an ObjectBinder object
   // ObjectBinder can make an relationship on nativeObject and zend_object
   // don't warry about memory, we do relase
   ObjectBinder *binder = new ObjectBinder(entry, nativeObject, abstractClsPrivatePtr->getObjectHandlers(), 1);
   return binder->getZendObject();
}

zend_object *AbstractClassPrivate::cloneObject(zval *value)
{
   return nullptr;
}

int AbstractClassPrivate::countElements(zval *object, zend_long *count)
{
   Countable *countable = dynamic_cast<Countable *>(ObjectBinder::retrieveSelfPtr(object)->getNativeObject());
   if (countable) {
      try {
         *count = countable->count();
         return ZAPI_SUCCESS;
      } catch (Exception &exception) {
         process_exception(exception);
         return ZAPI_FAILURE; // unreachable, prevent some compiler warning
      }
   } else {
      if (!std_object_handlers.count_elements) {
         return ZAPI_FAILURE;
      }
      return std_object_handlers.count_elements(object, count);
   }
}

zval *AbstractClassPrivate::readDimension(zval *object, zval *offset, int type, zval *returnValue)
{
   // what to do with the type?
   //
   // the type parameter tells us whether the dimension was read in READ
   // mode, WRITE mode, READWRITE mode or UNSET mode.
   //
   // In 99 out of 100 situations, it is called in regular READ mode (value 0),
   // only when it is called from a PHP script that has statements like
   // $x =& $object["x"], $object["x"]["y"] = "something" or unset($object["x"]["y"]),
   // the type parameter is set to a different value.
   //
   // But we must ask ourselves the question what we should be doing with such
   // cases. Internally, the object most likely has a full native implementation,
   // and the property that is returned is just a string or integer or some
   // other value, that is temporary WRAPPED into a zval to make it accessible
   // from PHP. If someone wants to get a reference to such an internal variable,
   // that is in most cases simply impossible.
   ArrayAccess *arrayAccess = dynamic_cast<ArrayAccess *>(ObjectBinder::retrieveSelfPtr(object)->getNativeObject());
   if (arrayAccess) {
      try {
         return toZval(arrayAccess->offsetGet(offset), type, returnValue);
      } catch (Exception &exception) {
         process_exception(exception);
         return nullptr; // unreachable, prevent some compiler warning
      }
   } else {
      if (std_object_handlers.read_dimension) {
         return nullptr;
      } else {
         return std_object_handlers.read_dimension(object, offset, type, returnValue);
      }
   }
}

void AbstractClassPrivate::writeDimension(zval *object, zval *offset, zval *value)
{
   ArrayAccess *arrayAccess = dynamic_cast<ArrayAccess *>(ObjectBinder::retrieveSelfPtr(object)->getNativeObject());
   if (arrayAccess) {
      try {
         arrayAccess->offsetSet(offset, value);
      } catch (Exception &exception) {
         process_exception(exception);
      }
   } else {
      if (std_object_handlers.write_dimension) {
         return;
      } else {
         std_object_handlers.write_dimension(object, offset, value);
      }
   }
}

int AbstractClassPrivate::hasDimension(zval *object, zval *offset, int checkEmpty)
{
   ArrayAccess *arrayAccess = dynamic_cast<ArrayAccess *>(ObjectBinder::retrieveSelfPtr(object)->getNativeObject());
   if (arrayAccess) {
      try {
         if (!arrayAccess->offsetExists(offset)) {
            return false;
         }
         if (!checkEmpty) {
            return true;
         }
         return zapi::empty(arrayAccess->offsetGet(offset));
      } catch (Exception &exception) {
         process_exception(exception);
         return false; // unreachable, prevent some compiler warning
      }
   } else {
      if (!std_object_handlers.has_dimension) {
         return false;
      }
      return std_object_handlers.has_dimension(object, offset, checkEmpty);
   }
}

void AbstractClassPrivate::unsetDimension(zval *object, zval *offset)
{
   ArrayAccess *arrayAccess = dynamic_cast<ArrayAccess *>(ObjectBinder::retrieveSelfPtr(object)->getNativeObject());
   if (arrayAccess) {
      try {
         arrayAccess->offsetUnset(offset);
      } catch (Exception &exception) {
         process_exception(exception);
      }
   } else {
      if (!std_object_handlers.unset_dimension) {
         return;
      }
      return std_object_handlers.unset_dimension(object, offset);
   }
}

zval *AbstractClassPrivate::readProperty(zval *object, zval *name, int type, void **cacheSlot, zval *rv)
{
   // what to do with the type?
   //
   // the type parameter tells us whether the property was read in READ
   // mode, WRITE mode, READWRITE mode or UNSET mode.
   //
   // In 99 out of 100 situations, it is called in regular READ mode (value 0),
   // only when it is called from a PHP script that has statements like
   // $x =& $object->x, $object->x->y = "something" or unset($object->x->y)
   // the type parameter is set to a different value.
   //
   // But we must ask ourselves the question what we should be doing with such
   // cases. Internally, the object most likely has a full native implementation,
   // and the property that is returned is just a string or integer or some
   // other value, that is temporary WRAPPED into a zval to make it accessible
   // from PHP. If someone wants to get a reference to such an internal variable,
   // that is in most cases simply impossible.
   // retrieve the object and class
   
   try {
      ObjectBinder *objectBinder = ObjectBinder::retrieveSelfPtr(object);
      AbstractClassPrivate *selfPtr = retrieve_acp_ptr_from_cls_entry(Z_OBJCE_P(object));
      AbstractClass *meta = selfPtr->m_apiPtr;
      StdClass *nativeObject = objectBinder->getNativeObject();
      std::string key(Z_STRVAL_P(name), Z_STRLEN_P(name));
      auto iter = selfPtr->m_properties.find(key);
      if (iter != selfPtr->m_properties.end()) {
         // self defined getter method
         return toZval(iter->second->get(nativeObject), type, rv);
      } else {
         return toZval(meta->callGet(nativeObject, key), type, rv);
      }
   } catch (const NotImplemented &exception) {
      if (!std_object_handlers.read_property) {
         // TODO here maybe problems
         return nullptr; 
      }
      return std_object_handlers.read_property(object, name, type, cacheSlot, rv);
   } catch (Exception &exception) {
      process_exception(exception);
      // this statement will never execute
      return nullptr;
   }
}

void AbstractClassPrivate::writeProperty(zval *object, zval *name, zval *value, void **cacheSlot)
{
   try {
      ObjectBinder *objectBinder = ObjectBinder::retrieveSelfPtr(object);
      AbstractClassPrivate *selfPtr = retrieve_acp_ptr_from_cls_entry(Z_OBJCE_P(object));
      AbstractClass *meta = selfPtr->m_apiPtr;
      StdClass *nativeObject = objectBinder->getNativeObject();
      std::string key(Z_STRVAL_P(name), Z_STRLEN_P(name));
      auto iter = selfPtr->m_properties.find(key);
      if (iter != selfPtr->m_properties.end()) {
         if (iter->second->set(nativeObject, value)) {
            return;
         }
         zend_error(E_ERROR, "Unable to write to read-only property %s", key.c_str());
      } else {
         meta->callSet(nativeObject, key, value);
      }
   } catch (const NotImplemented &exception) {
      if (!std_object_handlers.write_property) {
         return;
      }
      std_object_handlers.write_property(object, name, value, cacheSlot);
   } catch (Exception &exception) {
      process_exception(exception);
   }
}

int AbstractClassPrivate::hasProperty(zval *object, zval *name, int hasSetExists, void **cacheSlot)
{
   try {
      ObjectBinder *objectBinder = ObjectBinder::retrieveSelfPtr(object);
      AbstractClassPrivate *selfPtr = retrieve_acp_ptr_from_cls_entry(Z_OBJCE_P(object));
      AbstractClass *meta = selfPtr->m_apiPtr;
      StdClass *nativeObject = objectBinder->getNativeObject();
      std::string key(Z_STRVAL_P(name), Z_STRLEN_P(name));
      if (selfPtr->m_properties.find(key) != selfPtr->m_properties.end()) {
         return true;
      }
      if (!meta->callIsset(nativeObject, key)) {
         return false;
      }
      if (2 == hasSetExists) {
         return true;
      }
      Variant value = meta->callGet(nativeObject, key);
      if (0 == hasSetExists) {
         return value.getType() != Type::Null;
      } else {
         return value.toBool();
      }
   } catch (const NotImplemented &exception) {
      if (!std_object_handlers.has_property) {
         return false;
      }
      return std_object_handlers.has_property(object, name, hasSetExists, cacheSlot);
   } catch (Exception &exception) {
      process_exception(exception);
      return false; 
   }
}

void AbstractClassPrivate::unsetProperty(zval *object, zval *name, void **cacheSlot)
{
   try {
      ObjectBinder *objectBinder = ObjectBinder::retrieveSelfPtr(object);
      AbstractClassPrivate *selfPtr = retrieve_acp_ptr_from_cls_entry(Z_OBJCE_P(object));
      AbstractClass *meta = selfPtr->m_apiPtr;
      StdClass *nativeObject = objectBinder->getNativeObject();
      std::string key(Z_STRVAL_P(name), Z_STRLEN_P(name));
      if (selfPtr->m_properties.find(key) == selfPtr->m_properties.end()) {
         meta->callUnset(nativeObject, key);
         return;
      }
      zend_error(E_ERROR, "Property %s can not be unset", key.c_str());
   } catch (const NotImplemented &exception) {
      if (!std_object_handlers.unset_property) {
         return;
      }
      std_object_handlers.unset_property(object, name, cacheSlot);
   } catch (Exception &exception) {
      process_exception(exception);
   }
}

zend_function *AbstractClassPrivate::getMethod(zend_object **object, zend_string *method, const zval *key)
{
   zend_function *defaultFuncInfo = std_object_handlers.get_method(object, method, key);
   if (defaultFuncInfo) {
      return defaultFuncInfo;
   }
   zend_class_entry *entry = (*object)->ce;
   CallContext *callContext = reinterpret_cast<CallContext *>(emalloc(sizeof(CallContext)));
   zend_internal_function *func = &callContext->m_func;
   func->type = ZEND_INTERNAL_FUNCTION;
   func->module = nullptr;
   func->handler = &AbstractClassPrivate::magicCallForwarder;
   func->arg_info = nullptr;
   func->num_args = 0;
   func->required_num_args = 0;
   func->scope = entry;
   func->fn_flags = ZEND_ACC_CALL_VIA_HANDLER;
   func->function_name = method;
   callContext->m_selfPtr = retrieve_acp_ptr_from_cls_entry(entry);
   return reinterpret_cast<zend_function *>(callContext);
}

zend_function *AbstractClassPrivate::getStaticMethod(zend_class_entry *entry, zend_string *methodName)
{
   zend_function *defaultFuncInfo = zend_std_get_static_method(entry, methodName, nullptr);
   if (defaultFuncInfo) {
      return defaultFuncInfo;
   }
   // TODO here maybe have memory leak
   CallContext *callContext = reinterpret_cast<CallContext *>(emalloc(sizeof(CallContext)));
   zend_internal_function *func = &callContext->m_func;
   func->type = ZEND_INTERNAL_FUNCTION;
   func->module = nullptr;
   func->handler = &AbstractClassPrivate::magicCallForwarder;
   func->arg_info = nullptr;
   func->num_args = 0;
   func->required_num_args = 0;
   func->scope = nullptr;
   func->fn_flags = ZEND_ACC_CALL_VIA_HANDLER;
   func->function_name = methodName;
   callContext->m_selfPtr = retrieve_acp_ptr_from_cls_entry(entry);
   return reinterpret_cast<zend_function *>(callContext);
}

int AbstractClassPrivate::getClosure(zval *object, zend_class_entry **entry, zend_function **retFunc, 
                                     zend_object **objectPtr)
{
   // TODO here maybe have memory leak
   CallContext *callContext = reinterpret_cast<CallContext *>(emalloc(sizeof(CallContext)));
   zend_internal_function *func = &callContext->m_func;
   func->type = ZEND_INTERNAL_FUNCTION;
   func->module = nullptr;
   func->handler = &AbstractClassPrivate::magicInvokeForwarder;
   func->arg_info = nullptr;
   func->num_args = 0;
   func->required_num_args = 0;
   func->scope = *entry;
   func->fn_flags = ZEND_ACC_CALL_VIA_HANDLER;
   func->function_name = nullptr;
   callContext->m_selfPtr = retrieve_acp_ptr_from_cls_entry(Z_OBJCE_P(object));
   *retFunc = reinterpret_cast<zend_function *>(callContext);
   *objectPtr = Z_OBJ_P(object);
   return ZAPI_SUCCESS;
}

void AbstractClassPrivate::magicCallForwarder(INTERNAL_FUNCTION_PARAMETERS)
{
   CallContext *callContext = reinterpret_cast<CallContext *>(execute_data->func);
   zend_internal_function *func = &callContext->m_func;
   const char *name = ZSTR_VAL(func->function_name);
   ScopedFree scopeFree(callContext);
   try {
      AbstractClass *meta = callContext->m_selfPtr->m_apiPtr;
      Variant result(return_value, true);
      Parameters params(getThis(), ZEND_NUM_ARGS());
      StdClass *nativeObject = params.getObject();
      if (nativeObject) {
         result = meta->callMagicCall(nativeObject, name, params);
      } else {
         result = meta->callMagicStaticCall(name, params);
      }
   } catch (const NotImplemented &exception) {
      zend_error(E_ERROR, "Undefined method %s", name);
   } catch (Exception &exception) {
      process_exception(exception);
   }
}

void AbstractClassPrivate::magicInvokeForwarder(INTERNAL_FUNCTION_PARAMETERS)
{
   CallContext *callContext = reinterpret_cast<CallContext *>(execute_data->func);
   zend_internal_function *func = &callContext->m_func;
   AbstractClass *meta = callContext->m_selfPtr->m_apiPtr;
   ScopedFree scopeFree(callContext);
   try {
      Variant result(return_value, true);
      Parameters params(getThis(), ZEND_NUM_ARGS());
      StdClass *nativeObject = params.getObject();
      result = meta->callMagicInvoke(nativeObject, params);
   } catch (const NotImplemented &exception) {
      zend_error(E_ERROR, "Function name must be a string");
   } catch (Exception &exception) {
      process_exception(exception);
   }
}

int AbstractClassPrivate::cast(zval *object, zval *retValue, int type)
{
   ObjectBinder *objectBinder = ObjectBinder::retrieveSelfPtr(object);
   AbstractClassPrivate *selfPtr = retrieve_acp_ptr_from_cls_entry(Z_OBJCE_P(object));
   AbstractClass *meta = selfPtr->m_apiPtr;
   StdClass *nativeObject = objectBinder->getNativeObject();
   try {
      Variant result;
      switch (static_cast<Type>(type)) {
      case Type::Numeric:
         result = meta->castToInteger(nativeObject);
         break;
      case Type::Double:
         result = meta->castToDouble(nativeObject);
      case Type::Boolean:
         result = meta->castToBool(nativeObject);
      case Type::String:
         result = meta->castToString(nativeObject);
      default:
         throw NotImplemented();
         break;
      }
      ZVAL_DUP(retValue, result.getZvalPtr());
      return ZAPI_SUCCESS;
   } catch (const NotImplemented &exception) {
      if (!std_object_handlers.cast_object) {
         return ZAPI_FAILURE;
      }
      return std_object_handlers.cast_object(object, retValue, type);
   } catch (Exception &exception) {
      process_exception(exception);
      return ZAPI_FAILURE;
   }
}

int AbstractClassPrivate::compare(zval *left, zval *right)
{
   
}

void AbstractClassPrivate::destructObject(zend_object *object)
{
   ObjectBinder *binder = ObjectBinder::retrieveSelfPtr(object);
   AbstractClassPrivate *selfPtr = retrieve_acp_ptr_from_cls_entry(object->ce);
   try {
      StdClass *nativeObject = binder->getNativeObject();
      if (nativeObject) {
         selfPtr->m_apiPtr->callDestruct(nativeObject);
      }
   } catch (const NotImplemented &exception) {
      zend_objects_destroy_object(object);
   } catch (Exception &exception) {
      // a regular zapi::kernel::Exception was thrown by the extension, pass it on
      // to PHP user space
      process_exception(exception);
   }
}

void AbstractClassPrivate::freeObject(zend_object *object)
{
   ObjectBinder *binder = ObjectBinder::retrieveSelfPtr(object);
   binder->destroy();
}

zval *AbstractClassPrivate::toZval(Variant &&value, int type, zval *rv)
{
   zval result;
   if (type == 0 || value.getRefCount() <= 1) {
      result = value.detach(true);
   } else {
      // editable zval return a reference to it
      zval orig = value.detach(false);
      result = Variant(&orig, true).detach(true);
   }
   ZVAL_COPY_VALUE(rv, &result);
   return rv;
}
} // internal


AbstractClass::AbstractClass(const char *className, lang::ClassType type)
   : m_implPtr(std::make_shared<AbstractClassPrivate>(className, type))
{
}

AbstractClass::AbstractClass(const AbstractClass &other)
   : m_implPtr(other.m_implPtr)
{}

AbstractClass::AbstractClass(AbstractClass &&other) ZAPI_DECL_NOEXCEPT
   : m_implPtr(std::move(other.m_implPtr))
{}

AbstractClass &AbstractClass::operator=(const AbstractClass &other)
{
   if (this != &other) {
      m_implPtr = other.m_implPtr;
   }
   return *this;
}

AbstractClass::~AbstractClass()
{}

AbstractClass &AbstractClass::operator=(AbstractClass &&other) ZAPI_DECL_NOEXCEPT
{
   assert(this != &other);
   m_implPtr = std::move(other.m_implPtr);
   return *this;
}

void AbstractClass::registerInterface(const Interface &interface)
{
   ZAPI_D(AbstractClass);
   implPtr->m_interfaces.push_back(std::make_shared<Interface>(interface));
}

void AbstractClass::registerInterface(Interface &&interface)
{
   ZAPI_D(AbstractClass);
   implPtr->m_interfaces.push_back(std::make_shared<Interface>(std::move(interface)));
}

void AbstractClass::registerProperty(const char *name, std::nullptr_t, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<NullMember>(name, flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, int16_t value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<NumericMember>(name, value, 
                                                                flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, int32_t value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<NumericMember>(name, value, 
                                                                flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, int64_t value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<NumericMember>(name, value, 
                                                                flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, char value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<StringMember>(name, &value, 1, 
                                                               flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, const std::string &value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<StringMember>(name, value, 
                                                               flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, const char *value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<StringMember>(name, value, std::strlen(value), 
                                                               flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, bool value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<BoolMember>(name, value,
                                                             flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerProperty(const char *name, double value, Modifier flags)
{
   ZAPI_D(AbstractClass);
   implPtr->m_members.push_back(std::make_shared<FloatMember>(name, value,
                                                              flags & Modifier::PropertyModifiers));
}

void AbstractClass::registerConstant(const Constant &constant)
{
   ZAPI_D(AbstractClass);
   const zend_constant &zendConst = constant.getZendConstant();
   const std::string &name = constant.getName();
   switch (Z_TYPE(zendConst.value)) {
   case IS_NULL:
      registerProperty(name.c_str(), nullptr, Modifier::Const);
      break;
   case IS_LONG:
      registerProperty(name.c_str(), static_cast<int64_t>(Z_LVAL(zendConst.value)), Modifier::Const);
      break;
   case IS_DOUBLE:
      registerProperty(name.c_str(), Z_DVAL(zendConst.value), Modifier::Const);
      break;
   case IS_TRUE:
      registerProperty(name.c_str(), true, Modifier::Const);
      break;
   case IS_FALSE:
      registerProperty(name.c_str(), false, Modifier::Const);
      break;
   case IS_STRING:
      registerProperty(name.c_str(), std::string(Z_STRVAL(zendConst.value), Z_STRLEN(zendConst.value)), Modifier::Const);
      break;
   default:
      // this should not happend
      // but we workaround this
      // shadow copy
      zval copy;
      ZVAL_DUP(&copy, &zendConst.value);
      convert_to_string(&copy);
      registerProperty(name.c_str(), std::string(Z_STRVAL(copy), Z_STRLEN(copy)), Modifier::Const);
      break;
   }
}

void AbstractClass::registerMethod(const char *name, zapi::ZendCallable callable, 
                                   Modifier flags, const Arguments &args)
{
   m_implPtr->m_methods.push_back(std::make_shared<Method>(name, callable, (flags & Modifier::MethodModifiers), args));
}

// abstract
void AbstractClass::registerMethod(const char *name, Modifier flags, const Arguments &args)
{
   m_implPtr->m_methods.push_back(std::make_shared<Method>(name, (flags & Modifier::MethodModifiers) | Modifier::Abstract, args));
}

StdClass *AbstractClass::construct() const
{
   return nullptr;
}

StdClass *AbstractClass::clone() const
{
   return nullptr;
}

void AbstractClass::callClone(StdClass *nativeObject) const
{}

void AbstractClass::callDestruct(StdClass *nativeObject) const
{}

Variant AbstractClass::callMagicCall(StdClass *nativeObject, const char *name, Parameters &params) const
{
   return nullptr;
}

Variant AbstractClass::callMagicStaticCall(const char *name, Parameters &params) const
{
   return nullptr;
}

Variant AbstractClass::callMagicInvoke(StdClass *nativeObject, Parameters &params) const
{
   return nullptr;
}

Variant AbstractClass::callGet(StdClass *nativeObject, const std::string &name) const
{
   return nullptr;
}

void AbstractClass::callSet(StdClass *nativeObject, const std::string &name, 
                            const Variant &value) const
{}

bool AbstractClass::callIsset(StdClass *nativeObject, const std::string &name) const
{
   return false;
}

void AbstractClass::callUnset(StdClass *nativeObject, const std::string &name) const
{}

Variant AbstractClass::castToString(StdClass *nativeObject) const
{
   
}

Variant AbstractClass::castToInteger(StdClass *nativeObject) const
{
   
}

Variant AbstractClass::castToDouble(StdClass *nativeObject) const
{
   
}

Variant AbstractClass::castToBool(StdClass *nativeObject) const
{
   
}

bool AbstractClass::clonable() const
{
   return false;
}

bool AbstractClass::serializable() const
{
   return false;
}

bool AbstractClass::traversable() const
{
   return false;
}

zend_class_entry *AbstractClass::initialize(const std::string &prefix, int moduleNumber)
{
   return getImplPtr()->initialize(this, prefix, moduleNumber);
}

zend_class_entry *AbstractClass::initialize(int moduleNumber)
{
   return initialize("", moduleNumber);
}

void AbstractClass::notImplemented()
{
   throw NotImplemented();
}

} // vm
} // zapi