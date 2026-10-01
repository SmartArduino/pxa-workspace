# Board-private RPC701 coprocessor stack (see README.md).
#
# The board compiles these sources itself instead of registering a second
# ESP-IDF component: the pai-touch board directory is already the component
# listed in EXTRA_COMPONENT_DIRS, so nested component directories are never
# discovered. Paths are relative to the board component directory because
# idf_component_register() resolves SRCS against it.

set(PXA_RPC701_SRCS
    "vendor/rpc701/rpc_transfer.c"
    "vendor/rpc701/rpc_701.c"
    "vendor/rpc701/port_esp_os.c"
    "vendor/rpc701/protobuf-c/protobuf-c.c"
    "vendor/rpc701/protobuf/rpc_messages.pb-c.c"
    "vendor/rpc701/rpc_rsp.c"
    "vendor/rpc701/rpc_req.c"
    "vendor/rpc701/rpc_ota.c"
    "vendor/rpc701/rpc_evt.c"
    "vendor/rpc701/rpc_wrap.c"
    "vendor/rpc701/rpc_slave_if.c"
    "vendor/rpc701/rpc_music_cache.c"
    "vendor/rpc701/rpc_music_now.c")

# The vendor build uses exactly this include directory. rpc_messages.pb-c.h
# reaches protobuf-c through "protobuf-c/protobuf-c.h", so the vendor root has
# to stay on the include path.
set(PXA_RPC701_INCLUDE_DIR "vendor/rpc701")
