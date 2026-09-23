#pragma once

#include "embed/embed.h"
#include "api/object_builder.h"

#include <functional>
#include <span>
#include <string>

#include <memory>

namespace brokit::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

class HostClass {
public:
    struct Slots {
        std::unique_ptr<ev::Persistent> proto;
        std::unique_ptr<ev::Persistent> ctor;
    };

    void install(const char* name, uint32_t arity, ev::NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate = nullptr);

    void alias(const char* name) const;
    void inherit(const HostClass& base) const;

    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    void setStatic(const char* name, Value v) const;

    // The payload of a handle THIS class made; nullptr for anything else,
    // including another class's handle (a File is not unwrapped by the Blob
    // class: its payload has a different layout) or another library's.
    // ev::handleData answers for ANY handle, so every payload make() hands out
    // is registered with its class (host_class.cpp, brands). Allocates nothing.
    void* unwrap(Value val) const;
    bool isInstance(Value val) const { return unwrap(val) != nullptr; }

    Value prototype() const;
    Value constructor() const;

    bool installed() const;

private:
    Slots& slots() const;
    const Slots* slotsIfAny() const;
};

} // namespace brokit::api
