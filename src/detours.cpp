/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2024 Source2ZE
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

#include "networkbasetypes.pb.h"
#include "usercmd.pb.h"
#include "cs_usercmd.pb.h"

#include "cdetour.h"
#include "module.h"
#include "addresses.h"
#include "detours.h"
#include "entities.h"
#include "entity/ccsplayercontroller.h"
#include "entity/ccsplayerpawn.h"
#include "entity/cbasemodelentity.h"
#include "entity/ctriggerpush.h"
#include "entity/cgamerules.h"
#include "entity/services.h"
#include "playermanager.h"
#include "igameevents.h"
#include "gameconfig.h"
#include "serversideclient.h"
#include "tracefilter.h"

#define VPROF_ENABLED
#include "tier0/vprof.h"

#include "tier0/memdbgon.h"

extern CGlobalVars* GetGlobals();
extern CGameEntitySystem *g_pEntitySystem;
extern CCSGameRules *g_pGameRules;

CUtlVector<CDetourBase *> g_vecDetours;

DECLARE_DETOUR(TriggerPush_Touch, Detour_TriggerPush_Touch);
DECLARE_DETOUR(CTriggerGravity_GravityTouch, Detour_CTriggerGravity_GravityTouch);
DECLARE_DETOUR(ProcessUsercmds, Detour_ProcessUsercmds);

#define f32 float32
#define i32 int32_t
#define u32 uint32_t

CConVar<bool> g_cvarUseOldPush("cs2f_use_old_push", FCVAR_NONE, "Whether to use the old CSGO trigger_push behavior", false);
CConVar<bool> g_cvarLogPushes("cs2f_log_pushes", FCVAR_NONE, "Whether to log pushes (cs2f_use_old_push must be enabled)", false);

void FASTCALL Detour_TriggerPush_Touch(CTriggerPush* pPush, CBaseEntity* pOther)
{
	// This trigger pushes only once (and kills itself) or pushes only on StartTouch, both of which are fine already
	if (!g_cvarUseOldPush.Get() || pPush->m_spawnflags() & SF_TRIG_PUSH_ONCE || pPush->m_bTriggerOnStartTouch())
	{
		TriggerPush_Touch(pPush, pOther);
		return;
	}

	MoveType_t movetype = pOther->m_nActualMoveType();

	// VPhysics handling doesn't need any changes
	if (movetype == MOVETYPE_VPHYSICS)
	{
		TriggerPush_Touch(pPush, pOther);
		return;
	}

	if (movetype == MOVETYPE_NONE || movetype == MOVETYPE_PUSH || movetype == MOVETYPE_NOCLIP)
		return;

	CCollisionProperty* collisionProp = pOther->m_pCollision();
	if (!IsSolid(collisionProp->m_nSolidType(), collisionProp->m_usSolidFlags()))
		return;

	if (!pPush->PassesTriggerFilters(pOther))
		return;

	if (pOther->m_CBodyComponent()->m_pSceneNode()->m_pParent())
		return;

	Vector vecAbsDir;
	matrix3x4_t matTransform = pPush->m_CBodyComponent()->m_pSceneNode()->EntityToWorldTransform();

	Vector vecPushDir = pPush->m_vecPushDirEntitySpace();
	VectorRotate(vecPushDir, matTransform, vecAbsDir);

	Vector vecPush = vecAbsDir * pPush->m_flSpeed();

	uint32 flags = pOther->m_fFlags();

	if (flags & (1 << 23)) // TODO: is FL_BASEVELOCITY really gone?
		vecPush = vecPush + pOther->m_vecBaseVelocity();

	if (vecPush.z > 0 && (flags & FL_ONGROUND))
	{
		pOther->SetGroundEntity(nullptr);
		Vector origin = pOther->GetAbsOrigin();
		origin.z += 1.0f;

		pOther->Teleport(&origin, nullptr, nullptr);
	}

	if (g_cvarLogPushes.Get() && GetGlobals())
	{
		Vector vecEntBaseVelocity = pOther->m_vecBaseVelocity;
		Vector vecOrigPush = vecAbsDir * pPush->m_flSpeed();

		Message("Pushing entity %i | frame = %i | tick = %i | entity basevelocity %s = %.2f %.2f %.2f | original push velocity = %.2f %.2f %.2f | final push velocity = %.2f %.2f %.2f\n",
				pOther->GetEntityIndex(),
				GetGlobals()->framecount,
				GetGlobals()->tickcount,
				(flags & (1 << 23)) ? "WITH FLAG" : "",
				vecEntBaseVelocity.x, vecEntBaseVelocity.y, vecEntBaseVelocity.z,
				vecOrigPush.x, vecOrigPush.y, vecOrigPush.z,
				vecPush.x, vecPush.y, vecPush.z);
	}

	pOther->m_vecBaseVelocity(vecPush);

	flags |= (1 << 23); // TODO: is FL_BASEVELOCITY really gone?
	pOther->m_fFlags(flags);
}

void FASTCALL Detour_CTriggerGravity_GravityTouch(CBaseEntity* pEntity, CBaseEntity* pOther)
{
	// no need to call original function here
	// because original function calls CBaseEntity::SetGravityScale internal
	// but passes the wrong gravity scale value
	if (CTriggerGravityHandler::GravityTouching(pEntity, pOther))
		return;

	CTriggerGravity_GravityTouch(pEntity, pOther);
}

class CUserCmd
{
public:
	[[maybe_unused]] char pad0[0x10];
	CSGOUserCmdPB cmd;
	[[maybe_unused]] char pad1[0x38];
#ifdef PLATFORM_WINDOWS
	[[maybe_unused]] char pad2[0x8];
#endif
};

void* FASTCALL Detour_ProcessUsercmds(CCSPlayerController* pController, CUserCmd* cmds, int numcmds, bool paused, float margin)
{
	VPROF_SCOPE_BEGIN("Detour_ProcessUsercmds");

	for (int i = 0; i < numcmds; i++)
	{
		auto subtickMoves = cmds[i].cmd.mutable_base()->mutable_subtick_moves();
		auto iterator = subtickMoves->begin();

		while (iterator != subtickMoves->end())
		{
			uint64 button = iterator->button();

			// Remove normal subtick movement inputs by button & subtick movement viewangles by pitch/yaw
			if ((button >= IN_JUMP && button <= IN_MOVERIGHT && button != IN_USE) || iterator->pitch_delta() != 0.0f || iterator->yaw_delta() != 0.0f)
				subtickMoves->erase(iterator);
			else
				iterator++;
		}
	}

	VPROF_SCOPE_END();

	return ProcessUsercmds(pController, cmds, numcmds, paused, margin);
}


bool InitDetours(CGameConfig *gameConfig)
{
	bool success = true;

	FOR_EACH_VEC(g_vecDetours, i)
	{
		if (!g_vecDetours[i]->CreateDetour(gameConfig))
			success = false;
		
		g_vecDetours[i]->EnableDetour();
	}

	return success;
}

void FlushAllDetours()
{
	FOR_EACH_VEC_BACK(g_vecDetours, i)
	{
		g_vecDetours[i]->FreeDetour();
		g_vecDetours.FastRemove(i);
	}
}
