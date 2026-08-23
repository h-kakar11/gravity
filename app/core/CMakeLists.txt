# The mediatool-core executable: the sidecar process the Tauri (Rust) shell spawns and
# talks to over NDJSON on stdio (docs/ipc-contract.md). This is the ONLY C++ binary in
# the product -- everything else in core/ and engines/ is a library linked into it.
add_executable(mediatool-core main.cpp)

target_link_libraries(mediatool-core PRIVATE mtcore mtengines)
