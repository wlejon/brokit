#pragma once

#include "embed/embed.h"
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace brokit::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

struct HostProxyTraps {
    Value methods = ev::undefined();
    std::function<bool(const std::string& key, Value& out)> get;
    std::function<void(const std::string& key, Value v)> set;
    std::function<bool(const std::string& key)> has;
    std::function<std::vector<std::string>()> ownKeys;
    std::function<void(const std::string& key)> remove;
    Value target = ev::undefined();
    std::function<Value(Value thisValue, std::span<const Value> args)> apply;
    std::function<Value(std::span<const Value> args)> construct;
};

Value makeHostProxy(HostProxyTraps traps);

} // namespace brokit::api
