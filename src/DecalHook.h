#pragma once

namespace DecalHook
{
    // the detour on BGSDecalManager::ApplyDecal and the ticker that repairs land bodies and removes decals whole
    void Install();

    // SKSE messages: groups are dropped before a game loads
    void OnMessage(std::uint32_t a_type);
}
