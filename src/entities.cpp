/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2025 Source2ZE
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "entities.h"
#include "entity.h"

#include "entity/cbaseplayercontroller.h"
#include "entity/ccsplayercontroller.h"
#include "entity/ccsplayerpawn.h"

#include <unordered_map>
#include <unordered_set>

static constexpr uint32_t ENTITY_MURMURHASH_SEED = 0x97984357;
static constexpr uint32_t ENTITY_UNIQUE_INVALID = ~0u;

static uint32_t GetEntityUnique(CBaseEntity* pEntity)
{
	const auto& sUniqueHammerID = pEntity->m_sUniqueHammerID();
	if (sUniqueHammerID.IsEmpty())
		return ENTITY_UNIQUE_INVALID;

	return MurmurHash2LowerCase(sUniqueHammerID.Get(), ENTITY_MURMURHASH_SEED);
}

namespace CTriggerGravityHandler
{
	static std::unordered_map<uint32_t, float> s_gravityMap;

	void OnPrecache(CBaseEntity* pEntity, const CEntityKeyValues* kv)
	{
		const auto pGravity = kv->GetKeyValue("gravity");
		const auto pHammerId = kv->GetKeyValue("hammerUniqueId");
		if (!pGravity || !pHammerId)
			return;

		const auto flGravity = pGravity->GetFloat();
		const auto hEntity = MurmurHash2LowerCase(pHammerId->GetString(), ENTITY_MURMURHASH_SEED);

		s_gravityMap[hEntity] = flGravity;
	}

	bool GravityTouching(CBaseEntity* pEntity, CBaseEntity* pOther)
	{
		const auto hEntity = GetEntityUnique(pEntity);
		if (hEntity == ENTITY_UNIQUE_INVALID)
			return false;

		const auto gravity = s_gravityMap.find(hEntity);
		if (gravity == s_gravityMap.end())
			return false;

		if (pOther->IsPawn() && pOther->IsAlive())
			pOther->SetGravityScale(gravity->second);

		return true;
	}

	void OnEndTouch(CBaseEntity* pEntity, CBaseEntity* pOther)
	{
	}

	static void Shutdown()
	{
		s_gravityMap.clear();
	}
} // namespace CTriggerGravityHandler

void EntityHandler_OnEntitySpawned(CBaseEntity* pEntity)
{
}

void EntityHandler_OnLevelInit()
{
	CTriggerGravityHandler::Shutdown();
}