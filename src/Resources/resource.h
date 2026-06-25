//{{NO_DEPENDENCIES}}
// Microsoft Visual C++ generated include file.
// Used by ParticleEditor.rc
//
// The legacy native-UI dialog / icon / toolbar / menu / control / command IDs
// were removed in along with the Win32 UI. These five are the live IDs
// that remained from that block; more live resource IDs (ground / skydome
// textures, shaders, and shared error / query strings) follow further below.
#define IDI_LOGO                        109
#define IDB_MISSING                     120
#define IDB_GROUND                      130
#define IDS_SCENEHEAT                   133
#define IDR_DEFAULT_SHADER              140

// — alternate ground textures. The Grass/Sand/Snow slots no longer
// bundle the proprietary vanilla EaW textures; they load W_TEMPGRND00 /
// W_SAND00 / W_SNOW_RGH from the user's game install at runtime (see
// engine.cpp kGroundTextureGameLeaf), so IDs 142-144 are retired and NOT
// reused. IDB_GROUND_METAL/GREY were never wired to a resource — vestigial
// reserved IDs, kept to avoid renumbering (separate cleanup).
#define IDB_GROUND_METAL                145
#define IDB_GROUND_GREY                 146

// — texture-palette button glyph (16x16 24-bit BMP, painter's
// palette icon). Used as the BS_BITMAP image for IDC_BUTTON_PALETTE
// in the Textures groupbox header on the Appearance tab.
#define IDB_PALETTE_GLYPH               147

// — hover-pin badge (24x48 24-bit BMP, vertical strip).
// Top half (y=0..23) = empty/hover state; bottom half (y=24..47) =
// filled/pinned state. Blitted onto each cell when the user hovers.
#define IDB_PIN_BADGE                   148

// — skydome equirectangular shader. Samples an environment texture
// onto a UV sphere rendered from inside.
#define IDR_SHADER_SKYDOME              150

//: 8 bundled skydome textures (slots 1-8 in the picker dialog).
// RCDATA entries added in Task 5 will point at src/Resources/skydomes/*.tga.
#define IDR_SKYDOME_SPACE               151
#define IDR_SKYDOME_ATMOSPHERE          152
#define IDR_SKYDOME_SUNSET              153
#define IDR_SKYDOME_DAWN                154
#define IDR_SKYDOME_NIGHT               155
#define IDR_SKYDOME_OVERCAST            156
#define IDR_SKYDOME_STUDIO              157
#define IDR_SKYDOME_INDOOR              158

// — bump-mapped terrain lighting for the ground plane. Faithful port of
// the game's TerrainMeshBump.fx (reference/foc-shaders/) minus cloud/FOW.
#define IDR_SHADER_GROUND_LIT           159

// — shared error/query STRINGTABLE ids migrated here from the now-deleted
// Resources/resource.en.h when the legacy localized .rc/.h files were removed.
// These are the ONLY IDS_* the surviving engine/IO/host code LoadString()s
// (engine.cpp, files.cpp, xml.cpp, exceptions.h, main.cpp); their runtime
// strings live in the STRINGTABLE in ParticleEditor.rc (LANG_NEUTRAL). Numeric
// values are preserved from the original en.h. They share values with the
// like-numbered ICON/BITMAP ids above, but that is harmless: Win32 keys
// resources by (type, id, language), so a STRINGTABLE entry never collides
// with an ICON/BITMAP of the same numeric id.
#define IDS_ERROR_FILE_READ             101
#define IDS_ERROR_FILE_FIND             102
#define IDS_ERROR_FILE_WRITE            103
#define IDS_ERROR_FILE_CORRUPT          104
#define IDS_ERROR_FILE_FORMAT           105
#define IDS_ERROR_XML_PARSER_CREATE     106
#define IDS_ERROR_XML                   107
#define IDS_QUERY_DATA_PATH             109
#define IDS_ERROR_FILE_OPEN             134
#define IDS_ERROR_FILE_CREATE           136
#define IDS_ERROR_RENDERER_RESET        137


// Next default values for new objects
//
#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE        149
#define _APS_NEXT_COMMAND_VALUE         40086
#define _APS_NEXT_CONTROL_VALUE         1098
#define _APS_NEXT_SYMED_VALUE           101
#endif
#endif
