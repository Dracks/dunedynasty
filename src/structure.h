/** @file src/structure.h %Structure handling definitions. */

#ifndef STRUCTURE_H
#define STRUCTURE_H

#include <stdint.h>
#include "enumeration.h"
#include "buildqueue.h"
#include "object.h"

/** Available structure layouts. */
enum StructureLayout {
	STRUCTURE_LAYOUT_1x1 = 0,
	STRUCTURE_LAYOUT_2x1 = 1,
	STRUCTURE_LAYOUT_1x2 = 2,
	STRUCTURE_LAYOUT_2x2 = 3,
	STRUCTURE_LAYOUT_2x3 = 4,
	STRUCTURE_LAYOUT_3x2 = 5,
	STRUCTURE_LAYOUT_3x3 = 6,

	STRUCTURE_LAYOUT_MAX = 7
};

/** States a structure can be in */
enum StructureState {
	STRUCTURE_STATE_DETECT    = -2,                        /*!< Used when setting state, meaning to detect which state it has by looking at other properties. */
	STRUCTURE_STATE_JUSTBUILT = -1,                        /*!< This shows you the building animation etc. */
	STRUCTURE_STATE_IDLE      = 0,                         /*!< Structure is doing nothing. */
	STRUCTURE_STATE_BUSY      = 1,                         /*!< Structure is busy (harvester in refinery, unit in repair, .. */
	STRUCTURE_STATE_READY     = 2                          /*!< Structure is ready and unit will be deployed soon. */
};

/**
 * A Structure as stored in the memory.
 */
typedef struct Structure {
	Object o;                                               /*!< Common to Unit and Structures. */
	uint16 creatorHouseID;                                  /*!< The Index of the House who created this Structure. Required in case of take-overs. */
	uint16 rotationSpriteDiff;                              /*!< Which sprite to show for the current rotation of Turrets etc. */
	uint16 objectType;                                      /*!< Type of Unit/Structure we are building. */
	uint8  upgradeLevel;                                    /*!< The current level of upgrade of the Structure. */
	uint8  upgradeTimeLeft;                                 /*!< Time left before upgrade is complete, or 0 if no upgrade available. */
	uint16 countDown;                                       /*!< General countdown for various of functions. */
	uint16 buildCostRemainder;                              /*!< The remainder of the buildCost for next tick. */
	 int16 state;                                           /*!< The state of the structure. @see StructureState. */
	uint16 hitpointsMax;                                    /*!< Max amount of hitpoints. */

	enum SquadID squadID;
	uint16 rallyPoint;
	int factoryOffsetY;
	BuildQueue queue;
} Structure;

/**
 * Static information per Structure type.
 */
typedef struct StructureInfo {
	ObjectInfo o;                                           /*!< Common to UnitInfo and StructureInfo. */
	uint32 enterFilter;                                     /*!< Bitfield determining which unit is allowed to enter the structure. If bit n is set, then units of type n may enter */
	uint16 creditsStorage;                                  /*!< How many credits this Structure can store. */
	 int16 powerUsage;                                      /*!< How much power this Structure uses (positive value) or produces (negative value). */
	uint16 layout;                                          /*!< Layout type of Structure. */
	uint16 iconGroup;                                       /*!< In which IconGroup the sprites of the Structure belongs. */
	uint8  animationIndex[3];                               /*!< The index inside g_table_animation_structure for the Animation of the Structure. */
	uint8  buildableUnits[8];                               /*!< Which units this structure can produce. */
	uint16 upgradeCampaign[3][HOUSE_MAX];                   /*!< Minimum campaign for upgrades. */
	uint16 shortcut;
} StructureInfo;

/** X/Y pair defining a 2D size. */
typedef struct XYSize {
	uint16 width;  /*!< Horizontal length. */
	uint16 height; /*!< Vertical length. */
} XYSize;

struct House;
struct Widget;

extern const StructureInfo g_table_structureInfo_original[STRUCTURE_MAX];
extern StructureInfo g_table_structureInfo[STRUCTURE_MAX];
extern const uint16  g_table_structure_layoutTiles[STRUCTURE_LAYOUT_MAX][9];
extern const uint16  g_table_structure_layoutEdgeTiles[STRUCTURE_LAYOUT_MAX][8];
extern const uint16  g_table_structure_layoutTileCount[STRUCTURE_LAYOUT_MAX];
extern const tile32  g_table_structure_layoutTileDiff[STRUCTURE_LAYOUT_MAX];
extern const XYSize  g_table_structure_layoutSize[STRUCTURE_LAYOUT_MAX];
extern const int16   g_table_structure_layoutTilesAround[STRUCTURE_LAYOUT_MAX][16];

extern Structure *g_structureActive;
extern uint16 g_structureActivePosition;
extern uint16 g_structureActiveType;

extern void GameLoop_Structure(void);
extern uint8 Structure_StringToType(const char *name);
extern Structure *Structure_Create(uint16 index, uint8 typeID, uint8 houseID, uint16 position);
extern bool Structure_Place(Structure *s, uint16 position, enum HouseType houseID);
extern void Structure_CalculateHitpointsMax(struct House *h);
extern void Structure_Server_SetState(Structure *s, enum StructureState state);
extern bool Structure_SupportsRallyPoints(enum StructureType s);
extern uint16 Structure_Client_GetRallyPoint(const Structure *s, uint16 packed);
extern Structure *Structure_Get_ByPackedTile(uint16 packed);
extern uint32 Structure_GetStructuresBuilt(struct House *h);
extern int16 Structure_IsValidBuildLandscape(uint16 packed, enum StructureType type);
extern int16 Structure_IsValidBuildLocation(enum HouseType houseID, uint16 packed, enum StructureType type);
extern void Structure_Server_ActivateSpecial(Structure *s);
extern void Structure_RemoveFog(enum TileUnveilCause cause, const Structure *s);
extern bool Structure_Damage(Structure *s, uint16 damage, uint16 range);
extern bool Structure_IsUpgradable(Structure *s);
extern bool Structure_ConnectWall(uint16 position, bool recurse);
extern struct Unit *Structure_GetLinkedUnit(Structure *s);
extern void Structure_UntargetMe(Structure *s);
extern uint16 Structure_FindFreePosition(Structure *s, bool checkForSpice);
extern void Structure_Remove(Structure *s);
extern void Structure_Server_CancelBuild(Structure *s);
extern bool Structure_Server_BuildObject(Structure *s, uint16 objectType);
extern bool Structure_Server_SetUpgradingState(Structure *s, int state);
extern bool Structure_Server_SetRepairingState(Structure *s, int state);
extern void Structure_UpdateMap(Structure *s);
extern int Structure_GetAvailable_Starport(enum UnitType type);
extern int Structure_GetAvailable_ConstructionYard(const Structure *s, enum StructureType type);
extern int Structure_GetAvailable_Factory(const Structure *s, int i);
extern void Structure_InitFactoryItems(const Structure *s);
extern void Structure_HouseUnderAttack(uint8 houseID);

#endif /* STRUCTURE_H */
