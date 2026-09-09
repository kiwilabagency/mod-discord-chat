#
# mod-discord-chat: optional CMake integration
#
# This file is included automatically by the AzerothCore modules/CMakeLists.txt
# after the module targets have been created (see modules/CMakeLists.txt).
#
# The Discord client in this module uses Boost.Beast (header-only, already
# present through the AzerothCore Boost dependency) on top of Boost.Asio, and
# OpenSSL for TLS.  worldserver itself already links OpenSSL (auth/crypto), but
# a module (static "modules" library or standalone shared module) must declare
# the dependency explicitly so that the final link step pulls in libssl.
#

set(DISCORD_MODULE_TARGETS "")

# The module can be built either into the static "modules" library or as its
# own shared library whose project name is derived from the module folder.
if(TARGET modules)
    list(APPEND DISCORD_MODULE_TARGETS modules)
endif()

if(TARGET mod-discord-chat)
    list(APPEND DISCORD_MODULE_TARGETS mod-discord-chat)
endif()

if(TARGET mod_discord_chat)
    list(APPEND DISCORD_MODULE_TARGETS mod_discord_chat)
endif()

# Dynamic build project name (GetProjectNameOfModuleName -> mod_<name>).
if(TARGET mod_mod-discord-chat)
    list(APPEND DISCORD_MODULE_TARGETS mod_mod-discord-chat)
endif()

if(TARGET mod_mod_discord_chat)
    list(APPEND DISCORD_MODULE_TARGETS mod_mod_discord_chat)
endif()

foreach(DISCORD_TARGET ${DISCORD_MODULE_TARGETS})
    target_link_libraries(${DISCORD_TARGET}
        PRIVATE
            OpenSSL::SSL
            OpenSSL::Crypto)

    if(NOT TARGET OpenSSL::SSL AND OPENSSL_INCLUDE_DIR)
        target_include_directories(${DISCORD_TARGET}
            PRIVATE
                ${OPENSSL_INCLUDE_DIR})
    endif()
endforeach()

# Boost include dirs: usually propagated by acore-core-interface/game, but make
# sure they are visible when building as a standalone shared module.
if(TARGET mod-discord-chat OR TARGET mod_discord_chat OR TARGET mod_mod-discord-chat OR TARGET mod_mod_discord_chat)
    set(DISCORD_MODULE_TARGET)
    if(TARGET mod-discord-chat)
        set(DISCORD_MODULE_TARGET mod-discord-chat)
    elseif(TARGET mod_discord_chat)
        set(DISCORD_MODULE_TARGET mod_discord_chat)
    elseif(TARGET mod_mod-discord-chat)
        set(DISCORD_MODULE_TARGET mod_mod-discord-chat)
    else()
        set(DISCORD_MODULE_TARGET mod_mod_discord_chat)
    endif()

    if(Boost_INCLUDE_DIR)
        target_include_directories(${DISCORD_MODULE_TARGET}
            PRIVATE
                ${Boost_INCLUDE_DIR})
    endif()
endif()
