#ifndef PARTICLESYSTEM_H
#define PARTICLESYSTEM_H

#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstdint>
#include <functional>
#include "ChunkFile.h"

#include "files.h"

class EmitterInstance;
struct LinkExemptFlags;   // defined in LinkGroup.h; forward-declared to avoid pulling that header in

class ParticleSystem
{
public:
	// Group types
	static const int GT_EXACT        = 0;
	static const int GT_BOX          = 1;
	static const int GT_CUBE         = 2;
	static const int GT_SPHERE       = 3;
	static const int GT_CYLINDER     = 4;
	static const int NUM_GROUP_TYPES = 5;

	// Group IDs
	static const int GROUP_SPEED    = 0;
	static const int GROUP_LIFETIME = 1;
	static const int GROUP_POSITION = 2;
	static const int NUM_GROUPS     = 3;

	// Track IDs
	static const int TRACK_RED_CHANNEL    = 0;
	static const int TRACK_GREEN_CHANNEL  = 1;
	static const int TRACK_BLUE_CHANNEL   = 2;
	static const int TRACK_ALPHA_CHANNEL  = 3;
	static const int TRACK_SCALE          = 4;
	static const int TRACK_INDEX          = 5;
	static const int TRACK_ROTATION_SPEED = 6;
	static const int NUM_TRACKS           = 7;

    // Blend modes
    static const int BLEND_NONE                = 0;
    static const int BLEND_ADDITIVE            = 1;
    static const int BLEND_TRANSPARENT         = 2;
    static const int BLEND_INVERSE             = 3;
    static const int BLEND_DEPTH_ADDITIVE      = 4;
    static const int BLEND_DEPTH_TRANSPARENT   = 5;
    static const int BLEND_DEPTH_INVERSE       = 6;
    static const int BLEND_DIFFUSE_TRANSPARENT = 7;
    static const int BLEND_STENCIL_DARKEN      = 8;
    static const int BLEND_STENCIL_DARKEN_BLUR = 9;
    static const int BLEND_HEAT                = 10;
    static const int BLEND_BUMP                = 11;
    static const int BLEND_DECAL_BUMP          = 12;
    static const int BLEND_SCANLINES           = 13;

    // Ground behavior
    static const int GROUND_NONE      = 0;
    static const int GROUND_DISAPPEAR = 1;
    static const int GROUND_BOUNCE    = 2;
    static const int GROUND_STICK     = 3;

    // Emit mode
    static const int EMIT_DISABLE       = 0;
    static const int EMIT_RANDOM_VERTEX = 1;
    static const int EMIT_RANDOM_MESH   = 2;
    static const int EMIT_EVERY_VERTEX  = 3;

	class Emitter
	{
	public:
		struct Track
		{
			enum InterpolationType
			{
				IT_UNKNOWN = -1,
				IT_LINEAR  =  0,
				IT_SMOOTH  =  1,
				IT_STEP    =  2
			};

			struct Key
			{
				float value;
				float time;

                // Used for pure equality
                bool operator == (const Key& key) const { return time == key.time && value == key.value; }

                // Used for ordering in the set
				bool operator <  (const Key& key) const { return time < key.time; }

				Key() {}
				Key(const Key& k) : time(k.time), value(k.value) {}
				Key(float t, float v) : time(t), value(v) {}
			};

			typedef std::multiset<Key>	KeyMap;

			KeyMap			  keys;
			InterpolationType interpolation;
		};

		#pragma pack(1)
		struct Group
		{
			unsigned int type;
			float        minX, minY, minZ;
			float		 maxX, maxY, maxZ;
			float		 sideLength;
			float		 sphereRadius;
			unsigned int sphereEdge;
			float		 cylinderRadius;
			unsigned int cylinderEdge;
			float		 cylinderHeight;
			float		 valX, valY, valZ;
		};
		#pragma pack()

		// Emitter hierarchy. Exactly one child of each type per emitter,
		// not a list — the engine's runtime struct has a single 8-byte
		// pointer slot for each (`+0x1108` deathChild, `+0x1110` lifeChild,
		// proved from `StarWarsG.exe::FUN_14015ed60` and `EAW Terrain
		// Editor.exe::FUN_140134b50`, both 2968-byte writer functions).
		// See `tasks/multi_child_emitter_investigation.md`.
		size_t   spawnOnDeath;
		size_t   spawnDuringLife;
		Emitter* parent;
        bool     visible;   // Not stored, for use in editor only

        // Stable per-emitter identity (reorder glide). `index` is positional
        // and reshuffles on every structural change; `stableId` is assigned
        // once at construction from a process-monotonic counter and never
        // changes across reorder/reparent. Runtime-only — never persisted to
        // .alo, so undo/redo (which rebuilds emitters from a snapshot)
        // issues fresh ids. Surfaced on the emitters/list DTO so the React
        // tree can key rows by it. Assigned in ALL THREE constructors.
        unsigned int stableId;

        // Link-group membership. 0 = unlinked; non-zero IDs are
        // unique within a ParticleSystem and stable across save/load.
        // Persisted in an editor-only optional chunk (0x0100); ignored
        // by the game engine. Minimum group size is 2 — single-member
        // groups are not produced by any user-visible operation.
        uint32_t linkGroup;

		std::string name;
		std::string colorTexture;
		std::string normalTexture;

		// Random parameter groups
		Group groups[NUM_GROUPS];

		// Tracks
        // Use the 'tracks' array outside of this class. This way you can
        // alias several tracks to the same contents easily.
		Track  trackContents[NUM_TRACKS];
        Track* tracks[NUM_TRACKS];

		// Properties
	    bool  linkToSystem;
		bool  objectSpaceAcceleration;
		bool  doColorAddGrayscale;
		bool  affectedByWind;
		bool  isHeatParticle;
		bool  isWeatherParticle;
		bool  hasTail;
		bool  noDepthTest;
		bool  randomRotation;
		bool  randomRotationDirection;
		bool  isWorldOriented;
		bool  useBursts;
		int   emitFromMesh;
		float gravity;
		float lifetime;
		float initialDelay;
		float burstDelay;
		float inwardSpeed;
		float inwardAcceleration;
		float acceleration[3];
		float randomScalePerc;
		float randomLifetimePerc;
		float weatherCubeSize;
		float tailSize;
		float parentLinkStrength;
		float weatherCubeDistance;
		float randomRotationAverage;
		float randomRotationVariance;
		float randomColors[4];
		float bounciness;
		float freezeTime;
		float skipTime;
		float emitFromMeshOffset;
		float weatherFadeoutDistance;
		unsigned long nBursts;
		size_t        index;
		unsigned long blendMode;
		unsigned long textureSize;
		unsigned long nParticlesPerSecond;
		unsigned long nTriangles;
		unsigned long nParticlesPerBurst;
		unsigned long groundBehavior;

		// These have an unused function
		bool  unknown15;
		bool  unknown2b;
		bool  unknown44;
		float unknown11;
		float unknown3f;
		unsigned long unknown06;
		unsigned long unknown49;

        // We need to keep track of instances of this emitter type,
        // in case we delete this emitter type
        void registerEmitterInstance(EmitterInstance* instance);
        void unregisterEmitterInstance(EmitterInstance* instance);

        void write(ChunkWriter& writer) { write(writer, false); }
        void copy (ChunkWriter& writer) { write(writer, true); }

        // Detach this emitter from its link group (sets linkGroup = 0).
        // Caller is responsible for any group-bookkeeping side effects
        // (e.g. auto-dissolving a group that would otherwise have one
        // remaining member). See `LinkGroup.h` for the higher-level
        // helpers that handle that.
        void detachFromLinkGroup();

        // Overwrite this emitter's non-exempt parameters with `src`'s.
        // Used by the link-group propagation hook in `CaptureUndo`
        // (see src/main.cpp) and by the LinkGroup join helpers. The
        // exempt-flags struct lives in `LinkGroup.h`. Forward-declared
        // here to avoid pulling LinkGroup.h into ParticleSystem.h.
        //
        // Preserves on `*this`: m_instances, parent, spawnOnDeath,
        // spawnDuringLife, index, linkGroup, visible, stableId, plus
        // every exempt field (name, colorTexture, normalTexture, the
        // TRACK_INDEX keymap, depending on the flags supplied).
        //
        // Track aliasing is preserved: if `src` has multiple tracks
        // aliased to the same `trackContents[]` slot, `*this` will
        // end up with the same aliasing structure pointing into its
        // own `trackContents[]`. Note that TRACK_INDEX, when exempt,
        // is restored as a standalone (non-aliased) track on `*this`
        // — this matches the v1 expectation that the atlas-index
        // curve is intrinsically per-emitter.
        void copySharedParamsFrom(const Emitter& src,
                                   const struct LinkExemptFlags& exempt);

        Emitter(const Emitter& emitter);
		Emitter(ChunkReader& reader);
		Emitter();
        ~Emitter();

	private:
        std::set<EmitterInstance*> m_instances;

		void setDefaults();
        void write(ChunkWriter& writer, bool copy);

		void readProperties(ChunkReader& reader);
		void readTracks(ChunkReader& reader);
		void readGroups(ChunkReader& reader);

		void writeProperties(ChunkWriter& writer) const;
		void writeTracks(ChunkWriter& writer) const;
		void writeGroups(ChunkWriter& writer) const;
	};

	//
	// Functions
	//
	ParticleSystem();
	ParticleSystem(IFile* file);
	~ParticleSystem();

	// Write the particle system to a file
	void write(IFile* file);

	// Manage emitters
	Emitter*       addRootEmitter(const ParticleSystem::Emitter& emitter = ParticleSystem::Emitter());
    Emitter*       addLifetimeEmitter(Emitter* parent, const ParticleSystem::Emitter& emitter = ParticleSystem::Emitter());
    Emitter*       addDeathEmitter(Emitter* parent, const ParticleSystem::Emitter& emitter = ParticleSystem::Emitter());

    // Make the emitter spawn-graph well-formed after loading or importing
    // data that may be malformed: clear out-of-range spawn indices, drop
    // self-links and any child claimed by more than one parent, break
    // cycles, then rebuild parent pointers from the resulting forest.
    // deleteEmitter() and the EmitterList tree rebuild both recurse through
    // spawnOnDeath / spawnDuringLife and assume an acyclic single-parent
    // forest -- a cyclic or multi-parent graph would otherwise infinite-
    // recurse or double-free. Called from the ParticleSystem(IFile*) loader
    // (which also backs autosave restore) and the import-emitters helper.
    void ValidateEmitterGraph();

    // Clone the picked emitters (indices into `source`) into THIS system as
    // new roots: deep-copies each via the chunk serialiser (copy=true), re-maps
    // spawn links among the picked set (links to non-picked emitters drop to
    // -1), revalidates the merged graph via ValidateEmitterGraph (drops self /
    // duplicate-parent / cyclic links and rebuilds parents), and recreates
    // multi-member source link groups. `makeUniqueName` returns a collision-
    // free name for a clone given its source name — injected so the data layer
    // stays independent of the UI's GenerateDuplicateName. Returns the count
    // imported. Used by the host's import/duplicate bridge handler.
    size_t ImportEmittersFrom(
        ParticleSystem& source,
        const std::vector<size_t>& picks,
        const std::function<std::string(const std::string&)>& makeUniqueName);

    // Insert a copy of `source` directly after `reference` in m_emitters.
    // The new emitter becomes a root (parent=NULL, no spawn-children); existing
    // emitters at index >= reference->index + 1 shift up by one slot, with
    // their parent's spawn-field references updated to match. Used by the
    // "Duplicate Emitter" UI flow.
    Emitter*       insertEmitterAfter(const Emitter* reference, const ParticleSystem::Emitter& source);

    // Reorder a root emitter (and its full subtree) past the adjacent root
    // in the indicated direction. direction = -1 moves up (toward index 0),
    // +1 moves down. The emitter must be a root (parent == NULL); children
    // can't be reordered because each parent has fixed-role child slots
    // (spawnDuringLife / spawnOnDeath), not a sibling list.
    //
    // The whole subtree moves as a block: descendants reachable via
    // spawn-field traversal swap positions with the neighbor's subtree.
    // Emitters belonging to neither subtree stay where they are. All
    // spawn-field index references are updated to match the new layout.
    //
    // Returns true on success, false if the emitter isn't a root or there's
    // no neighboring root in the requested direction.
    bool           moveEmitter(Emitter* emitter, int direction);

    // Move `emitter` (must be a root) so its position in the root sequence
    // becomes `targetRootIndex` — i.e., the K-th emitter with parent==NULL,
    // counting from 0. The whole subtree moves as a block; spawn-field
    // indices on every affected parent are rewritten to match the new
    // layout, the same way moveEmitter does for adjacent swaps.
    //
    // Distinct from moveEmitter (which is a single neighbor-swap) so the
    // drag-and-drop reorder path can land at any target root index in one
    // shot rather than looping ±1 swaps and emitting intermediate states.
    //
    // Returns true on success. Returns false if the emitter isn't a root,
    // the target index is out of range (> count of roots), or the move
    // would be a no-op (target position equals current position).
    bool           moveEmitterToRootIndex(Emitter* emitter, size_t targetRootIndex);

    // Move a SET of root emitters so they become contiguous, landing at `gap`
    // (gap K = "before root K"; gap == rootCount = "after last root"),
    // preserving the selected roots' current top-to-bottom order. Non-
    // contiguous selections collapse together. `outNewIds` receives the moved
    // roots' final positional indices (a contiguous run, tree order). Returns
    // false on no-op / out-of-range / empty / non-root selection.
    bool           reorderManyRootsToIndex(const std::vector<Emitter*>& selection,
                                           size_t gap,
                                           std::vector<size_t>& outNewIds);

    // Reparent `source` so it becomes a child of `target` via target's
    // spawnDuringLife (when useSpawnDuringLife is true) or spawnOnDeath
    // (when false). The full subtree under source is preserved — its
    // children stay attached to source, and source's spawn-field
    // indices are unchanged. If source had a parent before, that
    // parent's spawn-slot reference to source is cleared to -1.
    //
    // Used by the drag-and-drop reparent gesture (drop emitter S onto
    // emitter T in the tree). Distinct from addLifetimeEmitter /
    // addDeathEmitter, which allocate a brand-new Emitter; this just
    // re-wires linkage on existing emitters.
    //
    // Returns true on success. Returns false (and leaves the system
    // untouched) if any of:
    //   - source or target is NULL, or source == target
    //   - target's chosen slot is currently occupied (not -1)
    //   - target is in source's subtree (would create a cycle)
    //   - source's current parent is target (slot-switching is out
    //     of scope; refused for the v1 reparent gesture)
    bool           reparentEmitter(Emitter* source, Emitter* target, bool useSpawnDuringLife);

    Emitter&       getEmitter(size_t index)       { return *m_emitters[index]; }
    const Emitter& getEmitter(size_t index) const { return *m_emitters[index]; }
	void           deleteEmitter(Emitter* emitter);

	// Getters
	const std::vector<Emitter*>& getEmitters()       const { return m_emitters; }
	      std::vector<Emitter*>& getEmitters()             { return m_emitters; }
	const std::string&           getName()           const { return m_name; }
	bool					 	 getLeaveParticles() const { return m_leaveParticles;  }

	// Setters
	void setName(const std::string& name) { m_name = name; }
	void setLeaveParticles(bool leave)    { m_leaveParticles = leave; }

    // per-group exempt-set storage. Groups not present in the
    // map use the v1 default exempt set (textures + atlas-index curve +
    // name) returned by GetDefaultLinkExemptFlags() in LinkGroup.cpp.
    // Persisted via the new system-body chunk 0x0003. Storage is sparse:
    // setLinkExemptFlags removes the entry if `flags` equals the v1
    // defaults, so files without per-group customization remain
    // byte-identical to the earlier output.
    const LinkExemptFlags& getLinkExemptFlags(uint32_t groupId) const;
    void                   setLinkExemptFlags(uint32_t                groupId,
                                              const LinkExemptFlags&  flags);

private:
	bool			 	                       m_leaveParticles;
	std::string                                m_name;
	std::vector<Emitter*>                      m_emitters;
    std::map<uint32_t, LinkExemptFlags>        m_linkExempts;
};
#endif