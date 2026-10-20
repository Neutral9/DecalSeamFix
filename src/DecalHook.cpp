#include "DecalHook.h"

#include <MinHook.h>

// A decal on the ground is built per land quadrant: the impact code (ID 44186) finds the land bodies the decal
// touches in the physics world and calls BGSDecalManager::ApplyDecal once for each, with the quadrant mesh it reads
// from the body (havok property 1 -> the quadrant's BSTriShape -> its parent mesh); each call clips the decal to that
// mesh. Vanilla loses pieces several ways, leaving half a decal that ends on a quadrant or cell edge:
//  1. with force false the engine drops a piece near an earlier decal on that mesh -> land pieces are forced;
//  2. it gives every piece the cell of the hit, and a save keeps no piece lying on another cell's land
//     -> a piece gets the cell whose land it lies on;
//  3. the land bodies of a cell that comes back into the grid from the cell buffer lose property 1, so the engine
//     gets no mesh for them -> the property is put back (and, should a call still come without a mesh, the piece
//     goes to every quadrant the decal reaches that no call of this impact went to);
//  4. old decals are removed mesh by mesh (cell unloads) -> when one piece goes, the rest of its decal goes too.
namespace DecalHook
{
    namespace
    {
        constexpr float kCellSize = 4096.0f;
        constexpr float kQuadSize = kCellSize * 0.5f;
        // the drawn decal is up to ~1.5x the texture set's max size (seen 127 for max 85): its half diagonal
        constexpr float kReachPerMaxSize = 1.5f * 0.5f * 1.4143f;

        std::string CellName(RE::TESObjectCELL* a_cell)
        {
            const auto xy = a_cell && a_cell->IsExteriorCell() ? a_cell->GetCoordinates() : nullptr;
            return xy ? std::format("({}, {})", xy->cellX, xy->cellY) : "?"s;
        }

        bool IsLandNode(const RE::NiAVObject* a_obj)
        {
            const auto rtti = a_obj ? a_obj->GetRTTI() : nullptr;
            return rtti && rtti->name && "BSMultiBoundNode"sv == rtti->name;
        }

        // the loaded cell whose land quadrant mesh a_obj is
        RE::TESObjectCELL* OwnerCell(const RE::NiAVObject* a_obj)
        {
            const auto tes = RE::TES::GetSingleton();
            const auto grid = tes ? tes->gridCells : nullptr;
            for (std::uint32_t i = 0; a_obj && grid && i < grid->length * grid->length; ++i) {
                const auto cell = grid->cells[i];
                const auto land = cell ? cell->GetRuntimeData().cellLand : nullptr;
                const auto loaded = land ? land->loadedData : nullptr;
                for (int q = 0; loaded && q < 4; ++q) {
                    if (loaded->mesh[q] == a_obj) {
                        return cell;
                    }
                }
            }
            return nullptr;
        }

        // --- 4. a decal's pieces go together ---

        struct Piece
        {
            RE::NiPointer<RE::BSTempEffect> effect;
            bool                            built = false;  // seen with its geometry on the mesh
        };
        struct Group
        {
            RE::NiPoint3       origin;
            std::vector<Piece> pieces;
        };
        constexpr std::size_t kMaxGroups = 4096;

        std::mutex        groupLock;
        std::deque<Group> groups;  // newest at the back
        // pieces we removed, held until nothing of the game refers to them any more
        std::deque<RE::NiPointer<RE::BSTempEffect>> graveyard;

        // our reference is the only one left: the game has let the piece go
        bool Gone(const RE::BSTempEffect* a_piece)
        {
            return a_piece->GetRefCount() <= 1;
        }

        // its geometry is built and hangs on a mesh's decal node (a piece on a quadrant the decal does not
        // reach gets none and is let go at once: that is no removal)
        bool Built(RE::BSTempEffect* a_piece)
        {
            const auto geometry = a_piece->initialized ? a_piece->Get3D() : nullptr;
            return geometry && geometry->parent;
        }

        void AddPiece(const RE::NiPoint3& a_origin, RE::BSTempEffect* a_piece)
        {
            std::scoped_lock lock(groupLock);
            if (groups.empty() || groups.back().origin.GetSquaredDistance(a_origin) >= 1.0f) {
                groups.push_back({ a_origin, {} });
                if (groups.size() > kMaxGroups) {
                    groups.pop_front();
                }
            }
            groups.back().pieces.push_back({ RE::NiPointer<RE::BSTempEffect>(a_piece) });
        }

        // loading clears every decal at once: no group may take that for the game removing a piece
        void ClearGroups()
        {
            std::scoped_lock lock(groupLock);
            groups.clear();
        }

        // takes a piece off its mesh: out of the decal node's list, its geometry detached (the game ignores a
        // lifetime on these decals). The manager's count is the game's: it still tracks the piece and counts it
        // down itself.
        void Remove(RE::BSTempEffect* a_piece)
        {
            const auto manager = RE::BGSDecalManager::GetSingleton();
            if (!manager) {
                return;
            }
            const auto geometry = a_piece->Get3D();
            for (const auto& node : manager->decalNodes) {
                if (!node) {
                    continue;
                }
                auto&      list = node->GetRuntimeData().decals;
                const auto it = std::ranges::find_if(list, [&](const auto& a_d) { return a_d.get() == a_piece; });
                if (it != list.end()) {
                    list.erase(it);
                    if (geometry && geometry->parent == node.get()) {
                        node->DetachChild(geometry);
                    }
                    return;
                }
            }
            if (geometry) {
                geometry->SetAppCulled(true);  // on no decal node: out of sight at least
            }
        }

        // on the main thread, a few times a second
        void CheckGroups()
        {
            std::scoped_lock lock(groupLock);
            for (auto it = groups.begin(); it != groups.end();) {
                // a decal on one mesh needs no watching (the newest group may still be growing)
                if (it->pieces.size() < 2 && std::next(it) != groups.end()) {
                    it = groups.erase(it);
                    continue;
                }
                bool removedBuilt = false;
                std::erase_if(it->pieces, [&](Piece& a_p) {
                    if (Gone(a_p.effect.get())) {
                        removedBuilt |= a_p.built;
                        return true;
                    }
                    a_p.built |= Built(a_p.effect.get());
                    return false;
                });
                if (!removedBuilt) {
                    it = it->pieces.empty() ? groups.erase(it) : std::next(it);
                    continue;
                }
                for (const auto& piece : it->pieces) {
                    Remove(piece.effect.get());
                    graveyard.push_back(piece.effect);
                }
                it = groups.erase(it);
            }
            std::erase_if(graveyard, [](const auto& a_p) { return Gone(a_p.get()); });
            while (graveyard.size() > kMaxGroups) {
                graveyard.pop_front();
            }
        }

        // --- 3. land bodies of cells back from the cell buffer get property 1 back ---
        // It goes not at once but when the cell is attached again, a few seconds after it enters the grid: such a
        // cell is watched for a while after each entry, a ray per quadrant and tick at a point where it last met the
        // quadrant's body. The land bodies cannot be reached from the 3D (no collision object on the quadrants).

        constexpr int  kLandLayer = 17;  // the land bodies' collision layer
        constexpr auto kWatchAfterEntry = 60s;

        struct CellEntry
        {
            int                                   enters = 0;
            std::chrono::steady_clock::time_point entered;
        };
        std::unordered_map<RE::TESObjectCELL*, CellEntry> entries;
        std::unordered_set<RE::TESObjectCELL*>            cellsInGrid;

        void TrackCells(RE::GridCellArray* a_grid)
        {
            std::unordered_set<RE::TESObjectCELL*> now;
            for (std::uint32_t i = 0; i < a_grid->length * a_grid->length; ++i) {
                if (a_grid->cells[i]) {
                    now.insert(a_grid->cells[i]);
                }
            }
            const auto time = std::chrono::steady_clock::now();
            for (const auto cell : now) {
                if (!cellsInGrid.contains(cell)) {
                    auto& entry = entries[cell];
                    ++entry.enters;
                    entry.entered = time;
                }
            }
            cellsInGrid = std::move(now);
        }

        // the root collidable a ray from a_top straight down a_down units hits first, its layer and where
        const RE::hkpCollidable* CastDown(RE::bhkWorld* a_world, const RE::NiPoint3& a_top, float a_down, int& a_layer, float& a_hitZ)
        {
            constexpr float kHavokScale = 0.0142875f;
            struct alignas(16) Buffer
            {
                Buffer()
                {
                    std::memset(static_cast<void*>(this), 0, sizeof(*this));
                    pick.rayOutput.Reset();
                }
                alignas(16) RE::bhkPickData pick;
                alignas(16) std::byte       spare[0x200];
            } buffer;
            auto& pick = buffer.pick;
            pick.rayInput.from = RE::hkVector4(a_top.x * kHavokScale, a_top.y * kHavokScale, a_top.z * kHavokScale, 0.0f);
            pick.rayInput.to = RE::hkVector4(a_top.x * kHavokScale, a_top.y * kHavokScale, (a_top.z - a_down) * kHavokScale, 0.0f);
            RE::CFilter filter{};
            filter.SetCollisionLayer(RE::COL_LAYER::kLOS);
            pick.rayInput.filterInfo = filter;
            {
                RE::BSReadLockGuard lock(a_world->worldLock);
                a_world->PickObject(pick);
            }
            const auto collidable = pick.rayOutput.HasHit() ? pick.rayOutput.rootCollidable : nullptr;
            a_layer = collidable ? static_cast<int>(collidable->broadPhaseHandle.collisionFilterInfo.GetCollisionLayer()) : -1;
            a_hitZ = a_top.z - a_down * pick.rayOutput.hitFraction;
            return collidable;
        }

        // as ID 44186 reads it: the collidable's owner -> userData (+18, the bhk wrapper) -> its havok object (+10)
        RE::hkpWorldObject* BodyOf(const RE::hkpCollidable* a_collidable)
        {
            const auto owner = static_cast<const std::byte*>(a_collidable->GetOwner());
            const auto bhk = owner ? *reinterpret_cast<const std::byte* const*>(owner + 0x18) : nullptr;
            return bhk ? *reinterpret_cast<RE::hkpWorldObject* const*>(bhk + 0x10) : nullptr;
        }

        // the land body under a_point, through whatever lies over the land (zones, rocks, trees)
        RE::hkpWorldObject* LandBodyAt(RE::bhkWorld* a_world, const RE::NiPoint3& a_point)
        {
            float       top = a_point.z + 3000.0f;
            const float bottom = a_point.z - 4000.0f;
            for (int i = 0; i < 6 && top > bottom; ++i) {
                int        layer = -1;
                float      hitZ = 0.0f;
                const auto collidable = CastDown(a_world, { a_point.x, a_point.y, top }, top - bottom, layer, hitZ);
                if (!collidable) {
                    return nullptr;
                }
                if (layer == kLandLayer) {
                    return BodyOf(collidable);
                }
                top = hitZ - 2.0f;
            }
            return nullptr;
        }

        // what property 1 points at on a healthy land body: the quadrant mesh's BSTriShape
        RE::NiAVObject* LandShape(RE::NiNode* a_mesh)
        {
            for (const auto& child : a_mesh->GetChildren()) {
                if (child && child->AsTriShape()) {
                    return child.get();
                }
            }
            return nullptr;
        }

        struct LandWatch
        {
            int                                        enters = 0;
            std::array<std::optional<RE::NiPoint3>, 4> point{};  // where the quadrant's body was met
        };
        std::unordered_map<RE::TESObjectCELL*, LandWatch> landWatch;
        int                                               landRepaired = 0;

        // whether the quadrant's body had to be repaired
        bool CheckQuadrant(RE::bhkWorld* a_world, RE::NiNode* a_mesh, int a_q, LandWatch& a_watch)
        {
            RE::hkpWorldObject* body = a_watch.point[a_q] ? LandBodyAt(a_world, *a_watch.point[a_q]) : nullptr;
            if (!body) {
                const auto& middle = a_mesh->worldBound.center;
                for (float dy : { 0.0f, -600.0f, 600.0f }) {
                    for (float dx : { 0.0f, -600.0f, 600.0f }) {
                        const RE::NiPoint3 at{ middle.x + dx, middle.y + dy, middle.z };
                        if (!body && (body = LandBodyAt(a_world, at))) {
                            a_watch.point[a_q] = at;
                        }
                    }
                }
            }
            if (!body || body->GetProperty(1)) {
                return false;
            }
            const auto shape = LandShape(a_mesh);
            if (!shape) {
                return false;
            }
            RE::BSWriteLockGuard lock(a_world->worldLock);
            body->SetProperty(1, RE::hkpPropertyValue(static_cast<void*>(shape)));
            return true;
        }

        void RepairLand(RE::GridCellArray* a_grid)
        {
            const auto now = std::chrono::steady_clock::now();
            for (std::uint32_t i = 0; i < a_grid->length * a_grid->length; ++i) {
                const auto cell = a_grid->cells[i];
                const auto land = cell ? cell->GetRuntimeData().cellLand : nullptr;
                const auto loaded = land ? land->loadedData : nullptr;
                const auto world = loaded ? cell->GetbhkWorld() : nullptr;
                const auto entry = world ? entries.find(cell) : entries.end();
                // a cell loaded fresh keeps its links; one back in the grid is watched for a while
                if (entry == entries.end() || entry->second.enters < 2 || now - entry->second.entered > kWatchAfterEntry) {
                    continue;
                }
                auto& watch = landWatch[cell];
                if (watch.enters != entry->second.enters) {
                    watch = LandWatch{ entry->second.enters };
                }
                int repaired = 0;
                for (int q = 0; q < 4; ++q) {
                    if (loaded->mesh[q] && CheckQuadrant(world, loaded->mesh[q], q, watch)) {
                        ++repaired;
                    }
                }
                if (repaired) {
                    landRepaired += repaired;
                    logger::info("cell {}: property 1 put back on {} land bod{} ({} in all)", CellName(cell), repaired, repaired == 1 ? "y" : "ies", landRepaired);
                }
            }
        }

        void Tick()
        {
            CheckGroups();
            const auto tes = RE::TES::GetSingleton();
            if (tes && tes->gridCells) {
                TrackCells(tes->gridCells);
                RepairLand(tes->gridCells);
            }
        }

        void StartTicker()
        {
            std::thread([] {
                for (;;) {
                    std::this_thread::sleep_for(250ms);
                    if (const auto tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] { Tick(); });
                    }
                }
            }).detach();
        }

        // --- 3b. a piece still without a mesh: to the quadrants the decal reaches ---

        // the meshes the pieces of the recent impacts went to (one impact's calls come one after another)
        struct Sent
        {
            RE::NiPoint3          origin;
            const RE::NiAVObject* node;
        };
        std::array<Sent, 32> sent{};
        std::size_t          sentNext = 0;
        int                  piecesAdded = 0;

        void Remember(const RE::NiPoint3& a_origin, const RE::NiAVObject* a_node)
        {
            sent[sentNext] = { a_origin, a_node };
            sentNext = (sentNext + 1) % sent.size();
        }

        bool WasSent(const RE::NiPoint3& a_origin, const RE::NiAVObject* a_node)
        {
            return std::ranges::any_of(sent, [&](const Sent& a_s) { return a_s.node == a_node && a_s.origin.GetSquaredDistance(a_origin) < 1.0f; });
        }

        struct Quadrant
        {
            RE::TESObjectCELL* cell;
            RE::NiNode*        mesh;
        };

        // the loaded land quadrants within a_reach of a_origin (in x, y)
        std::vector<Quadrant> QuadrantsNear(const RE::NiPoint3& a_origin, float a_reach)
        {
            std::vector<Quadrant> out;
            const auto            tes = RE::TES::GetSingleton();
            if (!tes) {
                return out;
            }
            const int x0 = static_cast<int>(std::floor((a_origin.x - a_reach) / kQuadSize));
            const int x1 = static_cast<int>(std::floor((a_origin.x + a_reach) / kQuadSize));
            const int y0 = static_cast<int>(std::floor((a_origin.y - a_reach) / kQuadSize));
            const int y1 = static_cast<int>(std::floor((a_origin.y + a_reach) / kQuadSize));
            for (int qy = y0; qy <= y1; ++qy) {
                for (int qx = x0; qx <= x1; ++qx) {
                    const RE::NiPoint3 middle{ (qx + 0.5f) * kQuadSize, (qy + 0.5f) * kQuadSize, a_origin.z };
                    const auto         cell = tes->GetCell(middle);
                    const auto         land = cell ? cell->GetRuntimeData().cellLand : nullptr;
                    const auto         loaded = land ? land->loadedData : nullptr;
                    // quadrant 0 at the cell's low x low y, 1 high x, 2 high y, 3 both
                    const int q = (qx & 1) + 2 * (qy & 1);
                    if (loaded && loaded->mesh[q]) {
                        out.push_back({ cell, loaded->mesh[q] });
                    }
                }
            }
            return out;
        }

        // --- the detour ---

        struct ApplyDecal
        {
            // one land piece: forced (1), given its land's cell (2), joined to its decal's group (4)
            static void* Piece(RE::BGSDecalManager* a_this, RE::DECAL_CREATION_DATA& a_data, RE::BGSDecalGroup* a_group, const RE::NiPoint3& a_origin)
            {
                const auto hitCell = a_data.parentCell;
                if (const auto owner = OwnerCell(a_data.avObj.get())) {
                    a_data.parentCell = owner;
                }
                const auto before = a_this->decals.size();
                const auto result = func(a_this, a_data, true, a_group);
                a_data.parentCell = hitCell;
                if (a_this->decals.size() > before && a_this->decals.back()) {
                    AddPiece(a_origin, a_this->decals.back().get());
                }
                return result;
            }

            static void* thunk(RE::BGSDecalManager* a_this, RE::DECAL_CREATION_DATA& a_data, bool a_force, RE::BGSDecalGroup* a_group)
            {
                const auto obj = a_data.avObj.get();
                const auto origin = a_data.origin;
                // a reference's decal, or one on something not land: the game's own way
                if (a_data.objRef.get() || (obj && !IsLandNode(obj))) {
                    return func(a_this, a_data, a_force, a_group);
                }
                if (obj) {
                    Remember(origin, obj);
                    return Piece(a_this, a_data, a_group, origin);
                }

                // no mesh (3b): every quadrant the decal reaches that no call of this impact went to
                const auto result = func(a_this, a_data, a_force, a_group);
                const auto texSet = a_data.texSet;
                if (!texSet || !texSet->decalData) {
                    return result;
                }
                const auto& size = texSet->decalData->data;
                const float reach = (std::max)(size.decalMaxWidth, size.decalMaxHeight) * kReachPerMaxSize;
                for (const auto& quadrant : QuadrantsNear(origin, reach)) {
                    if (WasSent(origin, quadrant.mesh)) {
                        continue;
                    }
                    Remember(origin, quadrant.mesh);
                    a_data.avObj.reset(quadrant.mesh);
                    Piece(a_this, a_data, a_group, origin);
                    if (++piecesAdded % 50 == 1) {
                        logger::info("pieces that came without a mesh, sent to the quadrants near them: {}", piecesAdded);
                    }
                }
                a_data.avObj.reset();
                return result;
            }

            static inline decltype(&thunk) func = nullptr;
        };
    }

    void OnMessage(std::uint32_t a_type)
    {
        if (a_type == SKSE::MessagingInterface::kPreLoadGame) {
            ClearGroups();
        }
    }

    void Install()
    {
        if (const auto init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("MinHook: {}", MH_StatusToString(init));
            return;
        }
        // BGSDecalManager::ApplyDecal
        const auto target = reinterpret_cast<void*>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(15029, 15203) }.address());
        auto       status = MH_CreateHook(target, reinterpret_cast<void*>(&ApplyDecal::thunk), reinterpret_cast<void**>(&ApplyDecal::func));
        if (status == MH_OK) {
            status = MH_EnableHook(target);
        }
        logger::info("game {} | ApplyDecal hook: {}", REL::Module::get().version().string(), MH_StatusToString(status));
        if (status == MH_OK) {
            StartTicker();
        }
    }
}
