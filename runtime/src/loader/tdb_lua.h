#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
struct Module;
bool run_tdb_autorun(const Module &main,
                     const std::filesystem::path &directory);
size_t prepare_player_probe(const Module &main,
                            const std::filesystem::path &directory);
void poll_player_request();
void on_player_update(uintptr_t context, uintptr_t manager);
