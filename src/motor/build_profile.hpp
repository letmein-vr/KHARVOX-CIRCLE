#pragma once
#include <cstdint>
#include <cstddef>

namespace circle::motor::build {
enum class Id { Unknown, Gog170, Steam170, GamePass170 };
// File: SHA-256 of the executable file. CodeImage: SHA-256 of the mapped .text section with every
// base-relocated pointer zeroed, for store builds whose file is encrypted on disk (Game Pass).
enum class Identity { File, CodeImage };
struct Profile { Id id; const char* name; Identity identity; const char* sha256; uint32_t timestamp, imageSize; };
// Exact content identity, not product version or store folder name.
const Profile* identify(Identity identity,const char* sha256,uint32_t timestamp,uint32_t imageSize);
const Profile* current();
bool supported();
// The SHA-256 the running executable was checked with, for logs ("" when none was computed).
const char* checkedHash();
uintptr_t resolve(Id id,uintptr_t canonical);
uintptr_t rva(uintptr_t canonical);
// Exact return-address entries only (no guessed relocation delta).
uintptr_t canonical(uintptr_t actual);
bool matches(uintptr_t canonical,const void* referenceBytes,size_t count);
}
