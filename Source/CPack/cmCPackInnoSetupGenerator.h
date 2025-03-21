/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
file LICENSE.rst or https://cmake.org/licensing for details. */

#pragma once

#include <map>
#include <string>
#include <vector>

#include "cmCPackGenerator.h"
#include "cmValue.h"

using cmCPackInnoSetupKeyValuePairs = std::map<std::string, std::string>;

class cmCPackComponentGroup;
class cmCPackComponent;

/** \class cmCPackInnoSetupGenerator
 * \brief A generator for Inno Setup
 *
 * https://jrsoftware.org/isinfo.php
 */
class cmCPackInnoSetupGenerator : public cmCPackGenerator
{
public:
  cmCPackTypeMacro(cmCPackInnoSetupGenerator, cmCPackGenerator);

  /**
   * Construct generator
   */
  cmCPackInnoSetupGenerator();
  ~cmCPackInnoSetupGenerator() override;

  static bool CanGenerate();

protected:
  int InitializeInternal() override;
  int PackageFiles() override;

  inline char const* GetOutputExtension() override { return ".exe"; }

  inline cmCPackGenerator::CPackSetDestdirSupport SupportsSetDestdir()
    const override
  {
    return cmCPackGenerator::SETDESTDIR_UNSUPPORTED;
  }

  inline bool SupportsAbsoluteDestination() const override { return false; }
  inline bool SupportsComponentInstallation() const override { return true; }

private:
  enum class PathType
  {
    Windows,
    Native,
  };

  bool ProcessSetupSection();
  bool ProcessFiles();
  bool ProcessComponents();

  bool ConfigureISScript();
  bool Compile();

  bool BuildDownloadedComponentArchive(cmCPackComponent* component,
                                       std::string const& uploadDirectory,
                                       std::string* hash);

  /**
   * Returns the option's value or an empty string if the option isn't set.
   */
  cmValue RequireOption(std::string const& key);

  std::string CustomComponentInstallDirectory(
    cmCPackComponent const* component);

  /**
   * Translates boolean expressions into "yes" or "no", as required in
   * Inno Setup (only if "CPACK_INNOSETUP_USE_CMAKE_BOOL_FORMAT" is on).
   */
  std::string TranslateBool(std::string const& value);

  /**
   * Creates a typical line of key and value pairs using the given map.
   *
   * (e.g.: Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}";
   * GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked)
   */
  std::string ISKeyValueLine(cmCPackInnoSetupKeyValuePairs const& params);

  std::string CreateRecursiveComponentPath(cmCPackComponentGroup* group,
                                           std::string const& path = "");

  void CreateRecursiveComponentGroups(cmCPackComponentGroup* group);

  /**
   * These functions add quotes if the given value hasn't already quotes.
   * Paths are converted into the format used by Windows before.
   */
  std::string Quote(std::string const& string);
  std::string QuotePath(std::string const& path,
                        PathType type = PathType::Windows);

  /**
   * This function replaces the following 5 characters with their %-encoding:
   * '|'  '}'  ','  '%'  '"'
   * Required for Inno Setup constants like {cm:...}
   */
  std::string PrepareForConstant(std::string const& string);

  std::vector<std::string> includeDirectives;
  cmCPackInnoSetupKeyValuePairs setupDirectives;
  bool toplevelProgramFolder;
  std::vector<std::string> languageInstructions;
  std::vector<std::string> fileInstructions;
  std::vector<std::string> dirInstructions;
  std::vector<std::string> typeInstructions;
  std::vector<std::string> componentInstructions;
  std::vector<std::string> iconInstructions;
  std::vector<std::string> desktopIconComponents;
  std::vector<std::string> runInstructions;
  std::vector<std::string> codeIncludes;
};
