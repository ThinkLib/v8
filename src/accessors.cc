// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/accessors.h"

#include "src/api.h"
#include "src/contexts.h"
#include "src/deoptimizer.h"
#include "src/execution.h"
#include "src/factory.h"
#include "src/frames-inl.h"
#include "src/isolate-inl.h"
#include "src/messages.h"
#include "src/property-details.h"
#include "src/prototype.h"

namespace v8 {
namespace internal {

Handle<AccessorInfo> Accessors::MakeAccessor(
    Isolate* isolate, Handle<Name> name, AccessorNameGetterCallback getter,
    AccessorNameBooleanSetterCallback setter, PropertyAttributes attributes) {
  Factory* factory = isolate->factory();
  Handle<AccessorInfo> info = factory->NewAccessorInfo();
  info->set_property_attributes(attributes);
  info->set_all_can_read(false);
  info->set_all_can_write(false);
  info->set_is_special_data_property(true);
  info->set_is_sloppy(false);
  info->set_replace_on_access(false);
  name = factory->InternalizeName(name);
  info->set_name(*name);
  Handle<Object> get = v8::FromCData(isolate, getter);
  if (setter == nullptr) setter = &ReconfigureToDataProperty;
  Handle<Object> set = v8::FromCData(isolate, setter);
  info->set_getter(*get);
  info->set_setter(*set);
  Address redirected = info->redirected_getter();
  if (redirected != nullptr) {
    Handle<Object> js_get = v8::FromCData(isolate, redirected);
    info->set_js_getter(*js_get);
  }
  return info;
}


static V8_INLINE bool CheckForName(Handle<Name> name,
                                   Handle<String> property_name,
                                   int offset,
                                   int* object_offset) {
  if (Name::Equals(name, property_name)) {
    *object_offset = offset;
    return true;
  }
  return false;
}


// Returns true for properties that are accessors to object fields.
// If true, *object_offset contains offset of object field.
bool Accessors::IsJSObjectFieldAccessor(Handle<Map> map, Handle<Name> name,
                                        int* object_offset) {
  Isolate* isolate = name->GetIsolate();

  switch (map->instance_type()) {
    case JS_ARRAY_TYPE:
      return
        CheckForName(name, isolate->factory()->length_string(),
                     JSArray::kLengthOffset, object_offset);
    default:
      if (map->instance_type() < FIRST_NONSTRING_TYPE) {
        return CheckForName(name, isolate->factory()->length_string(),
                            String::kLengthOffset, object_offset);
      }

      return false;
  }
}


namespace {

MUST_USE_RESULT MaybeHandle<Object> ReplaceAccessorWithDataProperty(
    Isolate* isolate, Handle<Object> receiver, Handle<JSObject> holder,
    Handle<Name> name, Handle<Object> value) {
  LookupIterator it(receiver, name, holder,
                    LookupIterator::OWN_SKIP_INTERCEPTOR);
  // Skip any access checks we might hit. This accessor should never hit in a
  // situation where the caller does not have access.
  if (it.state() == LookupIterator::ACCESS_CHECK) {
    CHECK(it.HasAccess());
    it.Next();
  }
  DCHECK(holder.is_identical_to(it.GetHolder<JSObject>()));
  CHECK_EQ(LookupIterator::ACCESSOR, it.state());
  it.ReconfigureDataProperty(value, it.property_attributes());
  return value;
}

}  // namespace

void Accessors::ReconfigureToDataProperty(
    v8::Local<v8::Name> key, v8::Local<v8::Value> val,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope stats_scope(
      isolate, &RuntimeCallStats::ReconfigureToDataProperty);
  HandleScope scope(isolate);
  Handle<Object> receiver = Utils::OpenHandle(*info.This());
  Handle<JSObject> holder =
      Handle<JSObject>::cast(Utils::OpenHandle(*info.Holder()));
  Handle<Name> name = Utils::OpenHandle(*key);
  Handle<Object> value = Utils::OpenHandle(*val);
  MaybeHandle<Object> result =
      ReplaceAccessorWithDataProperty(isolate, receiver, holder, name, value);
  if (result.is_null()) {
    isolate->OptionalRescheduleException(false);
  } else {
    info.GetReturnValue().Set(true);
  }
}

//
// Accessors::ArgumentsIterator
//


void Accessors::ArgumentsIteratorGetter(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* result = isolate->native_context()->array_values_iterator();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(result, isolate)));
}


Handle<AccessorInfo> Accessors::ArgumentsIteratorInfo(
    Isolate* isolate, PropertyAttributes attributes) {
  Handle<Name> name = isolate->factory()->iterator_symbol();
  return MakeAccessor(isolate, name, &ArgumentsIteratorGetter, nullptr,
                      attributes);
}


//
// Accessors::ArrayLength
//


void Accessors::ArrayLengthGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope timer(isolate, &RuntimeCallStats::ArrayLengthGetter);
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  JSArray* holder = JSArray::cast(*Utils::OpenHandle(*info.Holder()));
  Object* result = holder->length();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(result, isolate)));
}

void Accessors::ArrayLengthSetter(
    v8::Local<v8::Name> name, v8::Local<v8::Value> val,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope timer(isolate, &RuntimeCallStats::ArrayLengthSetter);
  HandleScope scope(isolate);

  DCHECK(Utils::OpenHandle(*name)->SameValue(isolate->heap()->length_string()));

  Handle<JSReceiver> object = Utils::OpenHandle(*info.Holder());
  Handle<JSArray> array = Handle<JSArray>::cast(object);
  Handle<Object> length_obj = Utils::OpenHandle(*val);

  bool was_readonly = JSArray::HasReadOnlyLength(array);

  uint32_t length = 0;
  if (!JSArray::AnythingToArrayLength(isolate, length_obj, &length)) {
    isolate->OptionalRescheduleException(false);
    return;
  }

  if (!was_readonly && V8_UNLIKELY(JSArray::HasReadOnlyLength(array)) &&
      length != array->length()->Number()) {
    // AnythingToArrayLength() may have called setter re-entrantly and modified
    // its property descriptor. Don't perform this check if "length" was
    // previously readonly, as this may have been called during
    // DefineOwnPropertyIgnoreAttributes().
    if (info.ShouldThrowOnError()) {
      Factory* factory = isolate->factory();
      isolate->Throw(*factory->NewTypeError(
          MessageTemplate::kStrictReadOnlyProperty, Utils::OpenHandle(*name),
          i::Object::TypeOf(isolate, object), object));
      isolate->OptionalRescheduleException(false);
    } else {
      info.GetReturnValue().Set(false);
    }
    return;
  }

  JSArray::SetLength(array, length);

  uint32_t actual_new_len = 0;
  CHECK(array->length()->ToArrayLength(&actual_new_len));
  // Fail if there were non-deletable elements.
  if (actual_new_len != length) {
    if (info.ShouldThrowOnError()) {
      Factory* factory = isolate->factory();
      isolate->Throw(*factory->NewTypeError(
          MessageTemplate::kStrictDeleteProperty,
          factory->NewNumberFromUint(actual_new_len - 1), array));
      isolate->OptionalRescheduleException(false);
    } else {
      info.GetReturnValue().Set(false);
    }
  } else {
    info.GetReturnValue().Set(true);
  }
}


Handle<AccessorInfo> Accessors::ArrayLengthInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate,
                      isolate->factory()->length_string(),
                      &ArrayLengthGetter,
                      &ArrayLengthSetter,
                      attributes);
}

//
// Accessors::ModuleNamespaceEntry
//

void Accessors::ModuleNamespaceEntryGetter(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  JSModuleNamespace* holder =
      JSModuleNamespace::cast(*Utils::OpenHandle(*info.Holder()));
  Handle<Object> result;
  if (!holder->GetExport(Handle<String>::cast(Utils::OpenHandle(*name)))
           .ToHandle(&result)) {
    isolate->OptionalRescheduleException(false);
  } else {
    info.GetReturnValue().Set(Utils::ToLocal(result));
  }
}

void Accessors::ModuleNamespaceEntrySetter(
    v8::Local<v8::Name> name, v8::Local<v8::Value> val,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Factory* factory = isolate->factory();
  Handle<JSModuleNamespace> holder =
      Handle<JSModuleNamespace>::cast(Utils::OpenHandle(*info.Holder()));

  if (info.ShouldThrowOnError()) {
    isolate->Throw(*factory->NewTypeError(
        MessageTemplate::kStrictReadOnlyProperty, Utils::OpenHandle(*name),
        i::Object::TypeOf(isolate, holder), holder));
    isolate->OptionalRescheduleException(false);
  } else {
    info.GetReturnValue().Set(false);
  }
}

Handle<AccessorInfo> Accessors::ModuleNamespaceEntryInfo(
    Isolate* isolate, Handle<String> name, PropertyAttributes attributes) {
  return MakeAccessor(isolate, name, &ModuleNamespaceEntryGetter,
                      &ModuleNamespaceEntrySetter, attributes);
}


//
// Accessors::StringLength
//

void Accessors::StringLengthGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope timer(isolate, &RuntimeCallStats::StringLengthGetter);
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);

  // We have a slight impedance mismatch between the external API and the way we
  // use callbacks internally: Externally, callbacks can only be used with
  // v8::Object, but internally we have callbacks on entities which are higher
  // in the hierarchy, in this case for String values.

  Object* value = *Utils::OpenHandle(*v8::Local<v8::Value>(info.This()));
  if (!value->IsString()) {
    // Not a string value. That means that we either got a String wrapper or
    // a Value with a String wrapper in its prototype chain.
    value = JSValue::cast(*Utils::OpenHandle(*info.Holder()))->value();
  }
  Object* result = Smi::FromInt(String::cast(value)->length());
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(result, isolate)));
}


Handle<AccessorInfo> Accessors::StringLengthInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->length_string(),
                      &StringLengthGetter, nullptr, attributes);
}


//
// Accessors::ScriptColumnOffset
//


void Accessors::ScriptColumnOffsetGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* res = Smi::FromInt(
      Script::cast(JSValue::cast(object)->value())->column_offset());
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptColumnOffsetInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
      STATIC_CHAR_VECTOR("column_offset")));
  return MakeAccessor(isolate, name, &ScriptColumnOffsetGetter, nullptr,
                      attributes);
}


//
// Accessors::ScriptId
//


void Accessors::ScriptIdGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* id = Smi::FromInt(Script::cast(JSValue::cast(object)->value())->id());
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(id, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptIdInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(
      isolate->factory()->InternalizeOneByteString(STATIC_CHAR_VECTOR("id")));
  return MakeAccessor(isolate, name, &ScriptIdGetter, nullptr, attributes);
}


//
// Accessors::ScriptName
//


void Accessors::ScriptNameGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* source = Script::cast(JSValue::cast(object)->value())->name();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(source, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptNameInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->name_string(),
                      &ScriptNameGetter, nullptr, attributes);
}


//
// Accessors::ScriptSource
//


void Accessors::ScriptSourceGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* source = Script::cast(JSValue::cast(object)->value())->source();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(source, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptSourceInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->source_string(),
                      &ScriptSourceGetter, nullptr, attributes);
}


//
// Accessors::ScriptLineOffset
//


void Accessors::ScriptLineOffsetGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* res =
      Smi::FromInt(Script::cast(JSValue::cast(object)->value())->line_offset());
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptLineOffsetInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
      STATIC_CHAR_VECTOR("line_offset")));
  return MakeAccessor(isolate, name, &ScriptLineOffsetGetter, nullptr,
                      attributes);
}


//
// Accessors::ScriptType
//


void Accessors::ScriptTypeGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* res =
      Smi::FromInt(Script::cast(JSValue::cast(object)->value())->type());
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptTypeInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(
      isolate->factory()->InternalizeOneByteString(STATIC_CHAR_VECTOR("type")));
  return MakeAccessor(isolate, name, &ScriptTypeGetter, nullptr, attributes);
}


//
// Accessors::ScriptCompilationType
//


void Accessors::ScriptCompilationTypeGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* res = Smi::FromInt(
      Script::cast(JSValue::cast(object)->value())->compilation_type());
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptCompilationTypeInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
      STATIC_CHAR_VECTOR("compilation_type")));
  return MakeAccessor(isolate, name, &ScriptCompilationTypeGetter, nullptr,
                      attributes);
}


//
// Accessors::ScriptSourceUrl
//


void Accessors::ScriptSourceUrlGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* url = Script::cast(JSValue::cast(object)->value())->source_url();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(url, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptSourceUrlInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->source_url_string(),
                      &ScriptSourceUrlGetter, nullptr, attributes);
}


//
// Accessors::ScriptSourceMappingUrl
//


void Accessors::ScriptSourceMappingUrlGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* url =
      Script::cast(JSValue::cast(object)->value())->source_mapping_url();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(url, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptSourceMappingUrlInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->source_mapping_url_string(),
                      &ScriptSourceMappingUrlGetter, nullptr, attributes);
}


//
// Accessors::ScriptGetContextData
//


void Accessors::ScriptContextDataGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.Holder());
  Object* res = Script::cast(JSValue::cast(object)->value())->context_data();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


Handle<AccessorInfo> Accessors::ScriptContextDataInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
      STATIC_CHAR_VECTOR("context_data")));
  return MakeAccessor(isolate, name, &ScriptContextDataGetter, nullptr,
                      attributes);
}


//
// Accessors::ScriptGetEvalFromScript
//


void Accessors::ScriptEvalFromScriptGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<Object> object = Utils::OpenHandle(*info.Holder());
  Handle<Script> script(
      Script::cast(Handle<JSValue>::cast(object)->value()), isolate);
  Handle<Object> result = isolate->factory()->undefined_value();
  if (!script->eval_from_shared()->IsUndefined(isolate)) {
    Handle<SharedFunctionInfo> eval_from_shared(
        SharedFunctionInfo::cast(script->eval_from_shared()));
    if (eval_from_shared->script()->IsScript()) {
      Handle<Script> eval_from_script(Script::cast(eval_from_shared->script()));
      result = Script::GetWrapper(eval_from_script);
    }
  }

  info.GetReturnValue().Set(Utils::ToLocal(result));
}


Handle<AccessorInfo> Accessors::ScriptEvalFromScriptInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
      STATIC_CHAR_VECTOR("eval_from_script")));
  return MakeAccessor(isolate, name, &ScriptEvalFromScriptGetter, nullptr,
                      attributes);
}


//
// Accessors::ScriptGetEvalFromScriptPosition
//


void Accessors::ScriptEvalFromScriptPositionGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<Object> object = Utils::OpenHandle(*info.Holder());
  Handle<Script> script(
      Script::cast(Handle<JSValue>::cast(object)->value()), isolate);
  Handle<Object> result = isolate->factory()->undefined_value();
  if (script->compilation_type() == Script::COMPILATION_TYPE_EVAL) {
    result = Handle<Object>(Smi::FromInt(script->GetEvalPosition()), isolate);
  }
  info.GetReturnValue().Set(Utils::ToLocal(result));
}


Handle<AccessorInfo> Accessors::ScriptEvalFromScriptPositionInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
      STATIC_CHAR_VECTOR("eval_from_script_position")));
  return MakeAccessor(isolate, name, &ScriptEvalFromScriptPositionGetter,
                      nullptr, attributes);
}


//
// Accessors::ScriptGetEvalFromFunctionName
//


void Accessors::ScriptEvalFromFunctionNameGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<Object> object = Utils::OpenHandle(*info.Holder());
  Handle<Script> script(
      Script::cast(Handle<JSValue>::cast(object)->value()), isolate);
  Handle<Object> result = isolate->factory()->undefined_value();
  if (!script->eval_from_shared()->IsUndefined(isolate)) {
    Handle<SharedFunctionInfo> shared(
        SharedFunctionInfo::cast(script->eval_from_shared()));
    // Find the name of the function calling eval.
    result = Handle<Object>(shared->name(), isolate);
  }
  info.GetReturnValue().Set(Utils::ToLocal(result));
}


Handle<AccessorInfo> Accessors::ScriptEvalFromFunctionNameInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
      STATIC_CHAR_VECTOR("eval_from_function_name")));
  return MakeAccessor(isolate, name, &ScriptEvalFromFunctionNameGetter, nullptr,
                      attributes);
}


//
// Accessors::FunctionPrototype
//

static Handle<Object> GetFunctionPrototype(Isolate* isolate,
                                           Handle<JSFunction> function) {
  if (!function->has_prototype()) {
    Handle<Object> proto = isolate->factory()->NewFunctionPrototype(function);
    JSFunction::SetPrototype(function, proto);
  }
  return Handle<Object>(function->prototype(), isolate);
}

void Accessors::FunctionPrototypeGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope timer(isolate,
                              &RuntimeCallStats::FunctionPrototypeGetter);
  HandleScope scope(isolate);
  Handle<JSFunction> function =
      Handle<JSFunction>::cast(Utils::OpenHandle(*info.Holder()));
  Handle<Object> result = GetFunctionPrototype(isolate, function);
  info.GetReturnValue().Set(Utils::ToLocal(result));
}

void Accessors::FunctionPrototypeSetter(
    v8::Local<v8::Name> name, v8::Local<v8::Value> val,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope timer(isolate,
                              &RuntimeCallStats::FunctionPrototypeSetter);
  HandleScope scope(isolate);
  Handle<Object> value = Utils::OpenHandle(*val);
  Handle<JSFunction> object =
      Handle<JSFunction>::cast(Utils::OpenHandle(*info.Holder()));
  JSFunction::SetPrototype(object, value);
  info.GetReturnValue().Set(true);
}


Handle<AccessorInfo> Accessors::FunctionPrototypeInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate,
                      isolate->factory()->prototype_string(),
                      &FunctionPrototypeGetter,
                      &FunctionPrototypeSetter,
                      attributes);
}


//
// Accessors::FunctionLength
//


void Accessors::FunctionLengthGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<JSFunction> function =
      Handle<JSFunction>::cast(Utils::OpenHandle(*info.Holder()));
  int length = 0;
  if (!JSFunction::GetLength(isolate, function).To(&length)) {
    isolate->OptionalRescheduleException(false);
  }
  Handle<Object> result(Smi::FromInt(length), isolate);
  info.GetReturnValue().Set(Utils::ToLocal(result));
}

Handle<AccessorInfo> Accessors::FunctionLengthInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->length_string(),
                      &FunctionLengthGetter, &ReconfigureToDataProperty,
                      attributes);
}


//
// Accessors::FunctionName
//


void Accessors::FunctionNameGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<JSFunction> function =
      Handle<JSFunction>::cast(Utils::OpenHandle(*info.Holder()));
  Handle<Object> result = JSFunction::GetName(isolate, function);
  info.GetReturnValue().Set(Utils::ToLocal(result));
}

Handle<AccessorInfo> Accessors::FunctionNameInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->name_string(),
                      &FunctionNameGetter, &ReconfigureToDataProperty,
                      attributes);
}


//
// Accessors::FunctionArguments
//


static Handle<Object> ArgumentsForInlinedFunction(
    JavaScriptFrame* frame,
    Handle<JSFunction> inlined_function,
    int inlined_frame_index) {
  Isolate* isolate = inlined_function->GetIsolate();
  Factory* factory = isolate->factory();

  TranslatedState translated_values(frame);
  translated_values.Prepare(frame->fp());

  int argument_count = 0;
  TranslatedFrame* translated_frame =
      translated_values.GetArgumentsInfoFromJSFrameIndex(inlined_frame_index,
                                                         &argument_count);
  TranslatedFrame::iterator iter = translated_frame->begin();

  // Skip the function.
  iter++;

  // Skip the receiver.
  iter++;
  argument_count--;

  Handle<JSObject> arguments =
      factory->NewArgumentsObject(inlined_function, argument_count);
  Handle<FixedArray> array = factory->NewFixedArray(argument_count);
  bool should_deoptimize = false;
  for (int i = 0; i < argument_count; ++i) {
    // If we materialize any object, we should deoptimize the frame because we
    // might alias an object that was eliminated by escape analysis.
    should_deoptimize = should_deoptimize || iter->IsMaterializedObject();
    Handle<Object> value = iter->GetValue();
    array->set(i, *value);
    iter++;
  }
  arguments->set_elements(*array);

  if (should_deoptimize) {
    translated_values.StoreMaterializedValuesAndDeopt(frame);
  }

  // Return the freshly allocated arguments object.
  return arguments;
}


static int FindFunctionInFrame(JavaScriptFrame* frame,
                               Handle<JSFunction> function) {
  std::vector<FrameSummary> frames;
  frame->Summarize(&frames);
  for (size_t i = frames.size(); i != 0; i--) {
    if (*frames[i - 1].AsJavaScript().function() == *function) {
      return static_cast<int>(i) - 1;
    }
  }
  return -1;
}


namespace {

Handle<Object> GetFunctionArguments(Isolate* isolate,
                                    Handle<JSFunction> function) {
  // Find the top invocation of the function by traversing frames.
  for (JavaScriptFrameIterator it(isolate); !it.done(); it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    int function_index = FindFunctionInFrame(frame, function);
    if (function_index < 0) continue;

    if (function_index > 0) {
      // The function in question was inlined.  Inlined functions have the
      // correct number of arguments and no allocated arguments object, so
      // we can construct a fresh one by interpreting the function's
      // deoptimization input data.
      return ArgumentsForInlinedFunction(frame, function, function_index);
    }

    // Find the frame that holds the actual arguments passed to the function.
    if (it.frame()->has_adapted_arguments()) {
      it.AdvanceOneFrame();
      DCHECK(it.frame()->is_arguments_adaptor());
    }
    frame = it.frame();

    // Get the number of arguments and construct an arguments object
    // mirror for the right frame.
    const int length = frame->ComputeParametersCount();
    Handle<JSObject> arguments = isolate->factory()->NewArgumentsObject(
        function, length);
    Handle<FixedArray> array = isolate->factory()->NewFixedArray(length);

    // Copy the parameters to the arguments object.
    DCHECK(array->length() == length);
    for (int i = 0; i < length; i++) {
      Object* value = frame->GetParameter(i);
      if (value->IsTheHole(isolate)) {
        // Generators currently use holes as dummy arguments when resuming.  We
        // must not leak those.
        DCHECK(IsResumableFunction(function->shared()->kind()));
        value = isolate->heap()->undefined_value();
      }
      array->set(i, value);
    }
    arguments->set_elements(*array);

    // Return the freshly allocated arguments object.
    return arguments;
  }

  // No frame corresponding to the given function found. Return null.
  return isolate->factory()->null_value();
}

}  // namespace


Handle<JSObject> Accessors::FunctionGetArguments(Handle<JSFunction> function) {
  Handle<Object> arguments =
      GetFunctionArguments(function->GetIsolate(), function);
  CHECK(arguments->IsJSObject());
  return Handle<JSObject>::cast(arguments);
}


void Accessors::FunctionArgumentsGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<JSFunction> function =
      Handle<JSFunction>::cast(Utils::OpenHandle(*info.Holder()));
  Handle<Object> result =
      function->shared()->native()
          ? Handle<Object>::cast(isolate->factory()->null_value())
          : GetFunctionArguments(isolate, function);
  info.GetReturnValue().Set(Utils::ToLocal(result));
}


Handle<AccessorInfo> Accessors::FunctionArgumentsInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->arguments_string(),
                      &FunctionArgumentsGetter, nullptr, attributes);
}


//
// Accessors::FunctionCaller
//


static inline bool AllowAccessToFunction(Context* current_context,
                                         JSFunction* function) {
  return current_context->HasSameSecurityTokenAs(function->context());
}


class FrameFunctionIterator {
 public:
  explicit FrameFunctionIterator(Isolate* isolate)
      : isolate_(isolate), frame_iterator_(isolate), inlined_frame_index_(-1) {
    GetFrames();
  }

  // Iterate through functions until the first occurrence of 'function'.
  // Returns true if one is found, and false if the iterator ends before.
  bool Find(Handle<JSFunction> function) {
    do {
      if (!next().ToHandle(&function_)) return false;
    } while (!function_.is_identical_to(function));
    return true;
  }

  // Iterate through functions until the next non-toplevel one is found.
  // Returns true if one is found, and false if the iterator ends before.
  bool FindNextNonTopLevel() {
    do {
      if (!next().ToHandle(&function_)) return false;
    } while (function_->shared()->is_toplevel());
    return true;
  }

  // Iterate through function until the first native or user-provided function
  // is found. Functions not defined in user-provided scripts are not visible
  // unless directly exposed, in which case the native flag is set on them.
  // Returns true if one is found, and false if the iterator ends before.
  bool FindFirstNativeOrUserJavaScript() {
    while (!function_->shared()->native() &&
           !function_->shared()->IsUserJavaScript()) {
      if (!next().ToHandle(&function_)) return false;
    }
    return true;
  }

  // In case of inlined frames the function could have been materialized from
  // deoptimization information. If that is the case we need to make sure that
  // subsequent call will see the same function, since we are about to hand out
  // the value to JavaScript. Make sure to store the materialized value and
  // trigger a deoptimization of the underlying frame.
  Handle<JSFunction> MaterializeFunction() {
    if (inlined_frame_index_ == 0) return function_;

    JavaScriptFrame* frame = frame_iterator_.frame();
    TranslatedState translated_values(frame);
    translated_values.Prepare(frame->fp());

    TranslatedFrame* translated_frame =
        translated_values.GetFrameFromJSFrameIndex(inlined_frame_index_);
    TranslatedFrame::iterator iter = translated_frame->begin();

    // First value is the function.
    bool should_deoptimize = iter->IsMaterializedObject();
    Handle<Object> value = iter->GetValue();
    if (should_deoptimize) {
      translated_values.StoreMaterializedValuesAndDeopt(frame);
    }

    return Handle<JSFunction>::cast(value);
  }

 private:
  MaybeHandle<JSFunction> next() {
    while (true) {
      inlined_frame_index_--;
      if (inlined_frame_index_ == -1) {
        if (!frame_iterator_.done()) {
          frame_iterator_.Advance();
          frames_.clear();
          GetFrames();
        }
        if (inlined_frame_index_ == -1) return MaybeHandle<JSFunction>();
        inlined_frame_index_--;
      }
      Handle<JSFunction> next_function =
          frames_[inlined_frame_index_].AsJavaScript().function();
      // Skip functions from other origins.
      if (!AllowAccessToFunction(isolate_->context(), *next_function)) continue;
      return next_function;
    }
  }
  void GetFrames() {
    DCHECK_EQ(-1, inlined_frame_index_);
    if (frame_iterator_.done()) return;
    JavaScriptFrame* frame = frame_iterator_.frame();
    frame->Summarize(&frames_);
    inlined_frame_index_ = static_cast<int>(frames_.size());
    DCHECK_LT(0, inlined_frame_index_);
  }
  Isolate* isolate_;
  Handle<JSFunction> function_;
  JavaScriptFrameIterator frame_iterator_;
  std::vector<FrameSummary> frames_;
  int inlined_frame_index_;
};


MaybeHandle<JSFunction> FindCaller(Isolate* isolate,
                                   Handle<JSFunction> function) {
  FrameFunctionIterator it(isolate);
  if (function->shared()->native()) {
    return MaybeHandle<JSFunction>();
  }
  // Find the function from the frames. Return null in case no frame
  // corresponding to the given function was found.
  if (!it.Find(function)) {
    return MaybeHandle<JSFunction>();
  }
  // Find previously called non-toplevel function.
  if (!it.FindNextNonTopLevel()) {
    return MaybeHandle<JSFunction>();
  }
  // Find the first user-land JavaScript function (or the entry point into
  // native JavaScript builtins in case such a builtin was the caller).
  if (!it.FindFirstNativeOrUserJavaScript()) {
    return MaybeHandle<JSFunction>();
  }

  // Materialize the function that the iterator is currently sitting on. Note
  // that this might trigger deoptimization in case the function was actually
  // materialized. Identity of the function must be preserved because we are
  // going to return it to JavaScript after this point.
  Handle<JSFunction> caller = it.MaterializeFunction();

  // Censor if the caller is not a sloppy mode function.
  // Change from ES5, which used to throw, see:
  // https://bugs.ecmascript.org/show_bug.cgi?id=310
  if (is_strict(caller->shared()->language_mode())) {
    return MaybeHandle<JSFunction>();
  }
  // Don't return caller from another security context.
  if (!AllowAccessToFunction(isolate->context(), *caller)) {
    return MaybeHandle<JSFunction>();
  }
  return caller;
}


void Accessors::FunctionCallerGetter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<JSFunction> function =
      Handle<JSFunction>::cast(Utils::OpenHandle(*info.Holder()));
  Handle<Object> result;
  MaybeHandle<JSFunction> maybe_caller;
  maybe_caller = FindCaller(isolate, function);
  Handle<JSFunction> caller;
  if (maybe_caller.ToHandle(&caller)) {
    result = caller;
  } else {
    result = isolate->factory()->null_value();
  }
  info.GetReturnValue().Set(Utils::ToLocal(result));
}


Handle<AccessorInfo> Accessors::FunctionCallerInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->caller_string(),
                      &FunctionCallerGetter, nullptr, attributes);
}


//
// Accessors::BoundFunctionLength
//

void Accessors::BoundFunctionLengthGetter(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope timer(isolate,
                              &RuntimeCallStats::BoundFunctionLengthGetter);
  HandleScope scope(isolate);
  Handle<JSBoundFunction> function =
      Handle<JSBoundFunction>::cast(Utils::OpenHandle(*info.Holder()));

  int length = 0;
  if (!JSBoundFunction::GetLength(isolate, function).To(&length)) {
    isolate->OptionalRescheduleException(false);
    return;
  }
  Handle<Object> result(Smi::FromInt(length), isolate);
  info.GetReturnValue().Set(Utils::ToLocal(result));
}

Handle<AccessorInfo> Accessors::BoundFunctionLengthInfo(
    Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->length_string(),
                      &BoundFunctionLengthGetter, &ReconfigureToDataProperty,
                      attributes);
}

//
// Accessors::BoundFunctionName
//

void Accessors::BoundFunctionNameGetter(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  RuntimeCallTimerScope timer(isolate,
                              &RuntimeCallStats::BoundFunctionNameGetter);
  HandleScope scope(isolate);
  Handle<JSBoundFunction> function =
      Handle<JSBoundFunction>::cast(Utils::OpenHandle(*info.Holder()));
  Handle<Object> result;
  if (!JSBoundFunction::GetName(isolate, function).ToHandle(&result)) {
    isolate->OptionalRescheduleException(false);
    return;
  }
  info.GetReturnValue().Set(Utils::ToLocal(result));
}

Handle<AccessorInfo> Accessors::BoundFunctionNameInfo(
    Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate, isolate->factory()->name_string(),
                      &BoundFunctionNameGetter, &ReconfigureToDataProperty,
                      attributes);
}

//
// Accessors::ErrorStack
//

namespace {

MaybeHandle<JSReceiver> ClearInternalStackTrace(Isolate* isolate,
                                                Handle<JSObject> error) {
  RETURN_ON_EXCEPTION(
      isolate,
      JSReceiver::SetProperty(error, isolate->factory()->stack_trace_symbol(),
                              isolate->factory()->undefined_value(),
                              LanguageMode::kStrict),
      JSReceiver);
  return error;
}

bool IsAccessor(Handle<Object> receiver, Handle<Name> name,
                Handle<JSObject> holder) {
  LookupIterator it(receiver, name, holder,
                    LookupIterator::OWN_SKIP_INTERCEPTOR);
  // Skip any access checks we might hit. This accessor should never hit in a
  // situation where the caller does not have access.
  if (it.state() == LookupIterator::ACCESS_CHECK) {
    CHECK(it.HasAccess());
    it.Next();
  }
  return (it.state() == LookupIterator::ACCESSOR);
}

}  // namespace

void Accessors::ErrorStackGetter(
    v8::Local<v8::Name> key, const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<JSObject> holder =
      Handle<JSObject>::cast(Utils::OpenHandle(*info.Holder()));

  // Retrieve the structured stack trace.

  Handle<Object> stack_trace;
  Handle<Symbol> stack_trace_symbol = isolate->factory()->stack_trace_symbol();
  MaybeHandle<Object> maybe_stack_trace =
      JSObject::GetProperty(holder, stack_trace_symbol);
  if (!maybe_stack_trace.ToHandle(&stack_trace) ||
      stack_trace->IsUndefined(isolate)) {
    Handle<Object> result = isolate->factory()->undefined_value();
    info.GetReturnValue().Set(Utils::ToLocal(result));
    return;
  }

  // Format it, clear the internal structured trace and reconfigure as a data
  // property.

  Handle<Object> formatted_stack_trace;
  if (!ErrorUtils::FormatStackTrace(isolate, holder, stack_trace)
           .ToHandle(&formatted_stack_trace)) {
    isolate->OptionalRescheduleException(false);
    return;
  }

  MaybeHandle<Object> result = ClearInternalStackTrace(isolate, holder);
  if (result.is_null()) {
    isolate->OptionalRescheduleException(false);
    return;
  }

  // If stack is still an accessor (this could have changed in the meantime
  // since FormatStackTrace can execute arbitrary JS), replace it with a data
  // property.
  Handle<Object> receiver =
      Utils::OpenHandle(*v8::Local<v8::Value>(info.This()));
  Handle<Name> name = Utils::OpenHandle(*key);
  if (IsAccessor(receiver, name, holder)) {
    result = ReplaceAccessorWithDataProperty(isolate, receiver, holder, name,
                                             formatted_stack_trace);
    if (result.is_null()) {
      isolate->OptionalRescheduleException(false);
      return;
    }
  } else {
    // The stack property has been modified in the meantime.
    if (!JSObject::GetProperty(holder, name).ToHandle(&formatted_stack_trace)) {
      isolate->OptionalRescheduleException(false);
      return;
    }
  }

  v8::Local<v8::Value> value = Utils::ToLocal(formatted_stack_trace);
  info.GetReturnValue().Set(value);
}

void Accessors::ErrorStackSetter(
    v8::Local<v8::Name> name, v8::Local<v8::Value> val,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<JSObject> obj = Handle<JSObject>::cast(
      Utils::OpenHandle(*v8::Local<v8::Value>(info.This())));

  // Clear internal properties to avoid memory leaks.
  Handle<Symbol> stack_trace_symbol = isolate->factory()->stack_trace_symbol();
  if (JSReceiver::HasOwnProperty(obj, stack_trace_symbol).FromMaybe(false)) {
    ClearInternalStackTrace(isolate, obj);
  }

  Accessors::ReconfigureToDataProperty(name, val, info);
}

Handle<AccessorInfo> Accessors::ErrorStackInfo(Isolate* isolate,
                                               PropertyAttributes attributes) {
  Handle<AccessorInfo> info =
      MakeAccessor(isolate, isolate->factory()->stack_string(),
                   &ErrorStackGetter, &ErrorStackSetter, attributes);
  return info;
}

}  // namespace internal
}  // namespace v8
