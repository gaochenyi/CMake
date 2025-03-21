/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#include "cmBinUtilsWindowsPEDumpbinGetRuntimeDependenciesTool.h"

#include <sstream>

#include <cmsys/RegularExpression.hxx>

#include "cmRuntimeDependencyArchive.h"
#include "cmUVProcessChain.h"
#include "cmUVStream.h"

cmBinUtilsWindowsPEDumpbinGetRuntimeDependenciesTool::
  cmBinUtilsWindowsPEDumpbinGetRuntimeDependenciesTool(
    cmRuntimeDependencyArchive* archive)
  : cmBinUtilsWindowsPEGetRuntimeDependenciesTool(archive)
{
}

bool cmBinUtilsWindowsPEDumpbinGetRuntimeDependenciesTool::GetFileInfo(
  std::string const& file, std::vector<std::string>& needed)
{
  cmUVProcessChainBuilder builder;
  builder.SetBuiltinStream(cmUVProcessChainBuilder::Stream_OUTPUT);

  std::vector<std::string> command;
  if (!this->Archive->GetGetRuntimeDependenciesCommand("dumpbin", command)) {
    this->SetError("Could not find dumpbin");
    return false;
  }
  command.emplace_back("/dependents");
  command.push_back(file);
  builder.AddCommand(command);

  auto process = builder.Start();
  if (!process.Valid() || process.GetStatus(0).SpawnResult != 0) {
    std::ostringstream e;
    e << "Failed to start dumpbin process for:\n  " << file;
    this->SetError(e.str());
    return false;
  }

  std::string line;
  static cmsys::RegularExpression const regex(
    "^    ([^\n]*\\.[Dd][Ll][Ll])\r$");
  cmUVPipeIStream output(process.GetLoop(), process.OutputStream());
  while (std::getline(output, line)) {
    cmsys::RegularExpressionMatch match;
    if (regex.find(line.c_str(), match)) {
      needed.push_back(match.match(1));
    }
  }

  if (!process.Wait()) {
    std::ostringstream e;
    e << "Failed to wait on dumpbin process for:\n  " << file;
    this->SetError(e.str());
    return false;
  }
  if (process.GetStatus(0).ExitStatus != 0) {
    std::ostringstream e;
    e << "Failed to run dumpbin on:\n  " << file;
    this->SetError(e.str());
    return false;
  }

  return true;
}
