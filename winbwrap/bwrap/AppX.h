#pragma once

#include <string>
#include <vector>

// Find an executable via "where.exe" PATH lookup.
// Returns the first matching path, or empty if not found.
std::wstring FindExePathFromPath(const std::wstring& exeName);

// Resolve a single App Execution Alias name to the folder that contains its
// real target executable.
std::wstring ResolveAliasTargetDir(const std::wstring& name);

// Check if a file path is an App Execution Alias (0-byte reparse point).
bool IsAppExecutionAlias(const std::wstring& filePath);
