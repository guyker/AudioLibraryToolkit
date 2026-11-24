#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "CommandLine.h"

namespace WorkerProtocol
{
using OperationRunner = std::function<int(const CommandLineOptions&, std::string&)>;

int Run(std::istream& input, std::ostream& output, const OperationRunner& runOperation);
}
