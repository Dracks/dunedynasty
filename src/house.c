/** @file src/house.c %House management routines. */

#include <assert.h>
#include <stdio.h>
#include "enum_string.h"
#include "os/math.h"
#include "os/strings.h"

#include "house.h"

#include "audio/audio.h"
#include "enhancement.h"
#include "gfx.h"
#include "gui/gui.h"
#include "gui/widget.h"
#include "map.h"
#include "mods/multiplayer.h"
#include "net/net.h"
#include "net/server.h"
#include "newui/actionpanel.h"
#include "newui/menubar.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/pool_house.h"
#include "pool/pool_structure.h"
#include "pool/pool_unit.h"
#include "scenario.h"
#include "string.h"
#include "structure.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "tools/encoded_index.h"
#include "tools/random_general.h"
#include "tools/random_lcg.h"
#include "tools/random_starport.h"
#include "unit.h"
#include "video/video.h"
#include "wsa.h"


House *g_playerHouse = NULL;
enum HouseType g_playerHouseID = HOUSE_INVALID;
uint16 g_playerCredits = 0; /*!< Credits shown to player as 'current'. */

static void House_EnsureHarvesterAvailable(uint8 houseID);
static void House_Server_TickMissileCountdown(House *h);

/**
 * Loop over all houses, preforming various of tasks.
 */
void GameLoop_House(void)
{
	PoolFindStruct find;
	bool tickHouse                = false;
	bool tickPowerMaintenance     = false;
	bool tickStarport             = false;
	bool tickReinforcement        = false;
	bool tickMissileCountdown     = false;
	bool tickStarportAvailability = false;

	if (g_debugScenario) return;

	if (g_tickHouseHouse <= g_timerGame) {
		tickHouse = true;
		g_tickHouseHouse = g_timerGame + 900;
	}

	if (g_tickHousePowerMaintenance <= g_timerGame) {
		tickPowerMaintenance = true;
		g_tickHousePowerMaintenance = g_timerGame + 10800;
	}

	if (g_tickHouseStarport <= g_timerGame) {
		tickStarport = true;
		g_tickHouseStarport = g_timerGame + 180;
	}

	if (g_tickHouseReinforcement <= g_timerGame) {
		tickReinforcement = true;
		g_tickHouseReinforcement = g_timerGame + (g_debugGame ? 60 : 600);
	}

	if (g_tickHouseMissileCountdown <= g_timerGame) {
		tickMissileCountdown = true;
		g_tickHouseMissileCountdown = g_timerGame + 60;
	}

	if (g_tickHouseStarportAvailability <= g_timerGame) {
		tickStarportAvailability = true;
		g_tickHouseStarportAvailability = g_timerGame + 1800;
	}

	if (g_tickHouseStarportRecalculatePrices <= g_timerGame) {
		const int64_t next_minute = Random_Starport_GetSeedTime() + 1;
		uint16 seed;

		if (g_host_type == HOSTTYPE_NONE) {
			seed = Random_Starport_GetSeed(g_scenarioID, g_playerHouseID);
		} else {
			seed = Random_Starport_GetSeed(g_multiplayer.curr_seed, 0);
		}

		g_tickHouseStarportRecalculatePrices = g_tickScenarioStart + next_minute * 60 * 60;
		Random_Starport_Seed(seed);
		g_factoryWindowTotal = -1;
	}

	if (tickMissileCountdown) {
		for (House *h = House_FindFirst(&find, HOUSE_INVALID);
				h != NULL;
				h = House_FindNext(&find)) {
			House_Server_TickMissileCountdown(h);
		}
	}

	if (tickStarportAvailability) {
		/* Pick a random unit to increase starport availability.
		 *
		 * If we are unlucky, we might restock something like a
		 * bullet, or unit that is already at maximum stock.  Not a
		 * bug; that's just how the game is.
		 */
		const enum UnitType type = Tools_RandomLCG_Range(0, UNIT_MAX - 1);
		Server_RestockStarport(type);
		g_factoryWindowTotal = -1;
	}

	if (tickReinforcement) {
		Unit *nu = NULL;
		int i;

		for (i = 0; i < 16; i++) {
			uint16 locationID;
			bool deployed;
			Unit *u;

			if (g_scenario.reinforcement[i].unitID == UNIT_INDEX_INVALID) continue;
			if (g_scenario.reinforcement[i].timeLeft == 0) continue;
			if (--g_scenario.reinforcement[i].timeLeft != 0) continue;

			u = Unit_Get_ByIndex(g_scenario.reinforcement[i].unitID);

			locationID = g_scenario.reinforcement[i].locationID;
			deployed   = false;

			if (locationID >= 4) {
				if (nu == NULL) {
					nu = Unit_Create(UNIT_INDEX_INVALID, UNIT_CARRYALL, u->o.houseID, Tile_UnpackTile(Map_Server_FindLocationTile(Tools_Random_256() & 3, u->o.houseID)), 100);

					if (nu != NULL) {
						nu->o.flags.s.byScenario = true;
						Unit_SetDestination(nu, Tools_Index_Encode(Map_Server_FindLocationTile(locationID, u->o.houseID), IT_TILE));
					}
				}

				if (nu != NULL) {
					u->o.linkedID = nu->o.linkedID;
					nu->o.linkedID = (uint8)u->o.index;
					nu->o.flags.s.inTransport = true;
					g_scenario.reinforcement[i].unitID = UNIT_INDEX_INVALID;
					deployed = true;
				} else {
					/* Failed to create carry-all, try again in a short moment */
					g_scenario.reinforcement[i].timeLeft = 1;
				}
			} else {
				deployed = Unit_SetPosition(u, Tile_UnpackTile(Map_Server_FindLocationTile(locationID, u->o.houseID)));
			}

			if (deployed && g_scenario.reinforcement[i].repeat != 0) {
				tile32 tile;
				tile.x = 0xFFFF;
				tile.y = 0xFFFF;

				g_validateStrictIfZero++;
				u = Unit_Create(UNIT_INDEX_INVALID, u->o.type, u->o.houseID, tile, 0);
				g_validateStrictIfZero--;

				if (u != NULL) {
					g_scenario.reinforcement[i].unitID = u->o.index;
					g_scenario.reinforcement[i].timeLeft = g_scenario.reinforcement[i].timeBetween;
				}
			}
		}
	}

	for (House *h = House_FindFirst(&find, HOUSE_INVALID);
			h != NULL;
			h = House_FindNext(&find)) {
		if (tickHouse) {
			/* ENHANCEMENT -- Originally this code was outside the house loop, which seems very odd.
			 *  This problem is considered to be so bad, that the original code has been removed. */
			uint16 maxCredits = max(h->creditsStorage, h->creditsStorageNoSilo);
			if (h->credits > maxCredits) {
				h->credits = maxCredits;

				Server_Send_StatusMessage1(1 << h->index, 1,
						STR_INSUFFICIENT_SPICE_STORAGE_AVAILABLE_SPICE_IS_LOST);
			}

			if (h->creditsStorage > h->creditsStorageNoSilo) {
				h->creditsStorageNoSilo = 0;
			}

			if (g_campaignID > 1
					&& h->creditsStorageNoSilo == 0
					&& h->credits != 0 && h->creditsStorage != 0
					&& (h->credits * 256 / h->creditsStorage) > 200) {
				Server_Send_StatusMessage1(1 << h->index, 0,
						STR_SPICE_STORAGE_CAPACITY_LOW_BUILD_SILOS);
			}

			if (h->credits < 100 && h->creditsStorageNoSilo != 0) {
				Server_Send_StatusMessage1(1 << h->index, 0,
						STR_CREDITS_ARE_LOW_HARVEST_SPICE_FOR_MORE_CREDITS);
			}
		}

		if (tickHouse) House_EnsureHarvesterAvailable(h->index);

		/* If h->starportLinkedID != UNIT_INDEX_INVALID and the queue
		 * is empty, that means we are waiting for the countdown
		 * before creating a frigate and delivering the units.
		 *
		 * ENHANCEMENT -- If no starports remaining, create a
		 * reinforcement carryall to reliably drop off the ordered
		 * units (at the home base).
		 */
		if (tickStarport
				&& h->starportLinkedID != UNIT_INDEX_INVALID
				&& BuildQueue_IsEmpty(&h->starportQueue)) {

			h->starportTimeLeft--;
			if ((int16)h->starportTimeLeft < 0) h->starportTimeLeft = 0;

			if (h->starportTimeLeft == 0) {
				const Structure *s = Structure_Get_ByIndex(h->starportID);
				Unit *u = NULL;

				if (s->o.type == STRUCTURE_STARPORT && s->o.houseID == h->index) {
					u = Unit_CreateWrapper(h->index, UNIT_FRIGATE,
							Tools_Index_Encode(s->o.index, IT_STRUCTURE));
				} else {
					PoolFindStruct find2;

					for (s = Structure_FindFirst(&find2, h->index, STRUCTURE_STARPORT);
							s != NULL;
							s = Structure_FindNext(&find2)) {
						if (s->o.linkedID != 0xFF) continue;

						u = Unit_CreateWrapper(h->index, UNIT_FRIGATE,
								Tools_Index_Encode(s->o.index, IT_STRUCTURE));
						break;
					}
				}

				if (g_dune2_enhanced && u == NULL) {
					const tile32 tile = Tile_UnpackTile(Map_Server_FindLocationTile(Tools_Random_256() & 3, h->index));

					u = Unit_Create(UNIT_INDEX_INVALID, UNIT_CARRYALL, h->index, tile, 100);
					if (u != NULL) {
						uint16 locationID = 7; /* Home Base. */
						const uint16 packed = Map_Server_FindLocationTile(locationID, h->index);

						u->o.flags.s.byScenario = true;
						Unit_SetDestination(u, Tools_Index_Encode(packed, IT_TILE));
					}
				}

				if (u != NULL) {
					u->o.linkedID = (uint8)h->starportLinkedID;
					h->starportLinkedID = UNIT_INDEX_INVALID;
					u->o.flags.s.inTransport = true;

					Server_Send_PlayVoice(1 << h->index,
							VOICE_FRIGATE_HAS_ARRIVED);

					h->starportTimeLeft
						= g_table_houseInfo[h->index].starportDeliveryTime;
				} else {
					h->starportTimeLeft = 1;
				}
			}
		}

		if (tickHouse) {
			House_CalculatePowerAndCredit(h);
			Structure_CalculateHitpointsMax(h);

			if (h->timerUnitAttack != 0) h->timerUnitAttack--;
			if (h->timerSandwormAttack != 0) h->timerSandwormAttack--;
			if (h->timerStructureAttack != 0) h->timerStructureAttack--;
			if (h->harvestersIncoming > 0 && Unit_CreateWrapper((uint8)h->index, UNIT_HARVESTER, 0) != NULL) h->harvestersIncoming--;
		}

		if (tickPowerMaintenance) {
			uint16 powerMaintenanceCost = (h->powerUsage / 32) + 1;
			h->credits -= min(h->credits, powerMaintenanceCost);
		}
	}
}

static void
House_Server_TickMissileCountdown(House *h)
{
	if (!h->flags.human || h->houseMissileCountdown == 0)
		return;

	h->houseMissileCountdown--;

	/* Count down is performed by client. */
	const enum VoiceID voiceID
		= VOICE_MISSILE_LAUNCHED + h->houseMissileCountdown - 1;

	if (voiceID == VOICE_MISSILE_LAUNCHED) {
		Server_Send_PlayVoice(1 << h->index, voiceID);
	}

	if (h->houseMissileCountdown == 0) {
		const uint16 packed = Map_Server_FindLocationTile(4, h->index);
		Unit_Server_LaunchHouseMissile(h, packed);
	}

	if (h == g_playerHouse) {
		House_Client_TickMissileCountdown();
	}
}

void
House_Client_TickMissileCountdown(void)
{
	const bool narrator_speaking = Audio_Poll();

	const enum VoiceID voiceID
		= VOICE_MISSILE_LAUNCHED + g_playerHouse->houseMissileCountdown - 1;

	if (!narrator_speaking
			&& (VOICE_ONE <= voiceID && voiceID <= VOICE_FIVE)) {
		Audio_PlayVoice(voiceID);
	} else if (voiceID == VOICE_FIVE) {
		Audio_PlaySound(EFFECT_COUNT_DOWN_TICK);
	}
}

/**
 * Convert the name of a house to the type value of that house, or
 *  HOUSE_INVALID if not found.
 */
uint8 House_StringToType(const char *name)
{
	uint8 index;
	if (name == NULL) return HOUSE_INVALID;

	for (index = 0; index < 6; index++) {
		if (strcasecmp(g_table_houseInfo[index].name, name) == 0) return index;
	}

	return HOUSE_INVALID;
}

/**
 * Gives a harvester to the given house if it has a refinery and no harvesters.
 *
 * @param houseID The index of the house to give a harvester to.
 */
static void House_EnsureHarvesterAvailable(uint8 houseID)
{
	PoolFindStruct find;
	const Structure *s;
	const Unit *u;

	for (s = Structure_FindFirst(&find, houseID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		/* ENHANCEMENT -- Dune2 checked the wrong type to skip. LinkedID is a structure for a Construction Yard */
		if (!g_dune2_enhanced && s->o.type == STRUCTURE_HEAVY_VEHICLE) continue;
		if (g_dune2_enhanced && s->o.type == STRUCTURE_CONSTRUCTION_YARD) continue;
		if (s->o.linkedID == UNIT_INVALID) continue;
		if (Unit_Get_ByIndex(s->o.linkedID)->o.type == UNIT_HARVESTER) return;
	}

	for (u = Unit_FindFirst(&find, houseID, UNIT_CARRYALL);
			u != NULL;
			u = Unit_FindNext(&find)) {
		if (u->o.linkedID == UNIT_INVALID) continue;
		if (Unit_Get_ByIndex(u->o.linkedID)->o.type == UNIT_HARVESTER) return;
	}

	u = Unit_FindFirst(&find, houseID, UNIT_HARVESTER);
	if (u != NULL) return;

	s = Structure_FindFirst(&find, houseID, STRUCTURE_REFINERY);
	if (s == NULL) return;

	if (Unit_CreateWrapper(houseID, UNIT_HARVESTER, Tools_Index_Encode(s->o.index, IT_STRUCTURE)) == NULL) return;

	Server_Send_StatusMessage1(1 << houseID, 0,
			STR_HARVESTER_IS_HEADING_TO_REFINERY);
}

bool
House_IsHuman(enum HouseType houseID)
{
	return House_Get_ByIndex(houseID)->flags.human;
}

enum HouseFlag
House_GetAIs(void)
{
	enum HouseFlag houses = 0;

	for (enum HouseType h = HOUSE_HARKONNEN; h < HOUSE_NEUTRAL; h++) {
		if (!House_IsHuman(h))
			houses |= (1 << h);
	}

	return houses;
}

/**
 * Checks if two houses are allied.
 *
 * @param houseID1 The index of the first house.
 * @param houseID2 The index of the second house.
 * @return True if and only if the two houses are allies of eachother.
 */
bool
House_AreAllied(enum HouseType houseID1, enum HouseType houseID2)
{
	assert(houseID1 < HOUSE_MAX);
	assert(houseID2 < HOUSE_MAX);

	if (g_table_houseAlliance[houseID1][houseID2] == HOUSEALLIANCE_ALLIES)
		return true;

	if (g_table_houseAlliance[houseID1][houseID2] == HOUSEALLIANCE_ENEMIES)
		return false;

	/* SINGLE PLAYER -- Alliances are set for the entire campaign, so
	 * need to check against the human player.  e.g. if playing
	 * Harkonnen, Atreides and Ordos allied.
	 */
	if (g_host_type == HOSTTYPE_NONE) {
		const bool h1_allied_to_player
			= (g_table_houseAlliance[houseID1][g_playerHouseID] == HOUSEALLIANCE_ALLIES);

		const bool h2_allied_to_player
			= (g_table_houseAlliance[houseID2][g_playerHouseID] == HOUSEALLIANCE_ALLIES);

		return (h1_allied_to_player == h2_allied_to_player);
	}

	return false;
}

enum HouseFlag
House_GetAllies(enum HouseType houseID)
{
	enum HouseFlag allies = 0;

	for (enum HouseType h = HOUSE_HARKONNEN; h < HOUSE_NEUTRAL; h++) {
		if (House_AreAllied(houseID, h))
			allies |= (1 << h);
	}

	return allies;
}

void
House_Server_ReassignToAI(enum HouseType houseID)
{
	House *h = House_Get_ByIndex(houseID);

	h->flags.human = false;
	h->flags.isAIActive = true;

	/* Might as well fix up all AI houses while we're at it. */
	const enum HouseFlag ai_houses = House_GetAIs();
	PoolFindStruct find;

	for (Structure *s = Structure_FindFirst(&find, HOUSE_INVALID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		s->o.seenByHouses |= ai_houses;
	}

	for (Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID);
			u != NULL;
			u = Unit_FindNext(&find)) {
		u->o.seenByHouses |= ai_houses;
	}
}

void
House_Server_Eliminate(enum HouseType houseID)
{

	PoolFindStruct find;

	for (Structure *s = Structure_FindFirst(&find, houseID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		Structure_Damage(s, s->o.hitpoints * 2, 0);
	}

	for (Unit *u = Unit_FindFirst(&find, houseID, UNIT_INVALID);
			u != NULL;
			u = Unit_FindNext(&find)) {
		Unit_Damage(u, u->o.hitpoints * 2, 0);
	}
}

void
House_Client_UpdateRadarState(void)
{
	House *h = g_playerHouse;

	const bool activate
		= (h->structuresBuilt & FLAG_STRUCTURE_OUTPOST)
		&& (h->powerProduction >= h->powerUsage);

	if (h->flags.radarActivated == activate)
		return;

	Audio_PlaySound(SOUND_RADAR_STATIC);
	Audio_PlayVoice(activate ? VOICE_RADAR_ACTIVATED : VOICE_RADAR_DEACTIVATED);
	MenuBar_StartRadarAnimation(activate);
	h->flags.radarActivated = activate;
}

/**
 * Update the CreditsStorage by walking over all structures and checking what
 *  they can hold.
 * @param houseID The house to check the storage for.
 */
void House_UpdateCreditsStorage(uint8 houseID)
{
	PoolFindStruct find;
	uint32 creditsStorage;

	uint16 oldValidateStrictIfZero = g_validateStrictIfZero;
	g_validateStrictIfZero = 0;

	creditsStorage = 0;

	for (const Structure *s = Structure_FindFirst(&find, houseID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		const StructureInfo *si = &g_table_structureInfo[s->o.type];
		creditsStorage += si->creditsStorage;
	}

	if (creditsStorage > 32000) creditsStorage = 32000;

	House_Get_ByIndex(houseID)->creditsStorage = creditsStorage;

	g_validateStrictIfZero = oldValidateStrictIfZero;
}

/**
 * Calculate the power usage and production, and the credits storage.
 *
 * @param h The house to calculate the numbers for.
 */
void House_CalculatePowerAndCredit(House *h)
{
	PoolFindStruct find;

	if (h == NULL) return;

	h->powerUsage      = 0;
	h->powerProduction = 0;
	h->creditsStorage  = 0;

	for (const Structure *s = Structure_FindFirst(&find, h->index, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		const StructureInfo *si = &g_table_structureInfo[s->o.type];

		/* ENHANCEMENT -- Only count structures that are placed on the map, not ones we are building. */
		if (g_dune2_enhanced && s->o.flags.s.isNotOnMap) continue;

		h->creditsStorage += si->creditsStorage;

		/* Positive values means usage */
		if (si->powerUsage >= 0) {
			h->powerUsage += si->powerUsage;
			continue;
		}

		/* Negative value and full health means everything goes to production */
		if (s->o.hitpoints >= si->o.hitpoints) {
			h->powerProduction += -si->powerUsage;
			continue;
		}

		/* Negative value and partial health, calculate how much should go to production (capped at 50%) */
		/* ENHANCEMENT -- The 50% cap of Dune2 is silly and disagress with the GUI. If your hp is 10%, so should the production. */
		if (!g_dune2_enhanced && s->o.hitpoints <= si->o.hitpoints / 2) {
			h->powerProduction += (-si->powerUsage) / 2;
			continue;
		}
		h->powerProduction += (-si->powerUsage) * s->o.hitpoints / si->o.hitpoints;
	}

	/* Check if we are low on power */
	if (h->powerUsage > h->powerProduction) {
		Server_Send_StatusMessage1(1 << h->index, 1,
				STR_INSUFFICIENT_POWER_WINDTRAP_IS_NEEDED);
	}

	/* If there are no buildings left, you lose your right on 'credits without storage'
	 * ENHANCEMENT -- check if we actually lost a structure, or if it was an MCV start.
	 */
	if (g_validateStrictIfZero == 0 && h->structuresBuilt == 0) {
		if (g_scenario.structuresLost[h->index] > 0)
			h->creditsStorageNoSilo = 0;
	}
}

bool
House_StarportQueueEmpty(const House *h)
{
	for (enum UnitType u = UNIT_CARRYALL; u < UNIT_MAX; u++) {
		if (h->starportCount[u] > 0)
			return false;
	}

	return true;
}

const char *House_GetWSAHouseFilename(uint8 houseID)
{
	static const char *houseWSAFileNames[HOUSE_NEUTRAL] = {
		"FHARK.WSA", "FARTR.WSA", "FORDOS.WSA",
		"FFREMN.WSA", "FSARD.WSA", "FMERC.WSA"
	};

	if (houseID >= HOUSE_NEUTRAL) return NULL;
	return houseWSAFileNames[houseID];
}

/*--------------------------------------------------------------*/

enum UnitType
House_GetInfantrySquad(enum HouseType houseID)
{
	if (houseID == HOUSE_ATREIDES)
		return UNIT_INFANTRY;

	return UNIT_TROOPERS;
}

enum UnitType
House_GetLightVehicle(enum HouseType houseID)
{
	if (houseID == HOUSE_ATREIDES)
		return UNIT_TRIKE;
	if (houseID == HOUSE_ORDOS)
		return UNIT_RAIDER_TRIKE;

	return UNIT_QUAD;
}

enum UnitType
House_GetIXVehicle(enum HouseType houseID)
{
	if (houseID == HOUSE_ATREIDES)
		return UNIT_SONIC_TANK;
	if (houseID == HOUSE_ORDOS)
		return UNIT_DEVIATOR;

	return UNIT_DEVASTATOR;
}
