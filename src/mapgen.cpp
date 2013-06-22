/*
Minetest
Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "mapgen.h"
#include "voxel.h"
#include "noise.h"
#include "biome.h"
#include "mapblock.h"
#include "mapnode.h"
#include "map.h"
//#include "serverobject.h"
#include "content_sao.h"
#include "nodedef.h"
#include "content_mapnode.h" // For content_mapnode_get_new_name
#include "voxelalgorithms.h"
#include "profiler.h"
#include "settings.h" // For g_settings
#include "main.h" // For g_profiler
#include "treegen.h"
#include "mapgen_v6.h"
#include "mapgen_v7.h"
#include "util/serialize.h"

FlagDesc flagdesc_mapgen[] = {
	{"trees",          MG_TREES},
	{"caves",          MG_CAVES},
	{"dungeons",       MG_DUNGEONS},
	{"v6_jungles",     MGV6_JUNGLES},
	{"v6_biome_blend", MGV6_BIOME_BLEND},
	{"flat",           MG_FLAT},
	{NULL,             0}
};

FlagDesc flagdesc_ore[] = {
	{"absheight",            OREFLAG_ABSHEIGHT},
	{"scatter_noisedensity", OREFLAG_DENSITY},
	{"claylike_nodeisnt",    OREFLAG_NODEISNT},
	{NULL,                   0}
};

FlagDesc flagdesc_deco_schematic[] = {
	{"place_center_x", DECO_PLACE_CENTER_X},
	{"place_center_y", DECO_PLACE_CENTER_Y},
	{"place_center_z", DECO_PLACE_CENTER_Z},
	{NULL,             0}
};

///////////////////////////////////////////////////////////////////////////////


Ore *createOre(OreType type) {
	switch (type) {
		case ORE_SCATTER:
			return new OreScatter;
		case ORE_SHEET:
			return new OreSheet;
		//case ORE_CLAYLIKE: //TODO: implement this!
		//	return new OreClaylike;
		default:
			return NULL;
	}
}


Ore::~Ore() {
	delete np;
	delete noise;
}


void Ore::resolveNodeNames(INodeDefManager *ndef) {
	if (ore == CONTENT_IGNORE) {
		ore = ndef->getId(ore_name);
		if (ore == CONTENT_IGNORE) {
			errorstream << "Ore::resolveNodeNames: ore node '"
				<< ore_name << "' not defined";
			ore     = CONTENT_AIR;
			wherein = CONTENT_AIR;
		}
	}

	if (wherein == CONTENT_IGNORE) {
		wherein = ndef->getId(wherein_name);
		if (wherein == CONTENT_IGNORE) {
			errorstream << "Ore::resolveNodeNames: wherein node '"
				<< wherein_name << "' not defined";
			ore     = CONTENT_AIR;
			wherein = CONTENT_AIR;
		}
	}
}


void Ore::placeOre(Mapgen *mg, u32 blockseed, v3s16 nmin, v3s16 nmax) {
	int in_range = 0;

	in_range |= (nmin.Y <= height_max && nmax.Y >= height_min);
	if (flags & OREFLAG_ABSHEIGHT)
		in_range |= (nmin.Y >= -height_max && nmax.Y <= -height_min) << 1;
	if (!in_range)
		return;

	int ymin, ymax;
	if (in_range & ORE_RANGE_MIRROR) {
		ymin = MYMAX(nmin.Y, -height_max);
		ymax = MYMIN(nmax.Y, -height_min);
	} else {
		ymin = MYMAX(nmin.Y, height_min);
		ymax = MYMIN(nmax.Y, height_max);
	}
	if (clust_size >= ymax - ymin + 1)
		return;

	nmin.Y = ymin;
	nmax.Y = ymax;
	generate(mg->vm, mg->seed, blockseed, nmin, nmax);
}


void OreScatter::generate(ManualMapVoxelManipulator *vm, int seed,
						  u32 blockseed, v3s16 nmin, v3s16 nmax) {
	PseudoRandom pr(blockseed);
	MapNode n_ore(ore, 0, ore_param2);

	int volume = (nmax.X - nmin.X + 1) *
				 (nmax.Y - nmin.Y + 1) *
				 (nmax.Z - nmin.Z + 1);
	int csize     = clust_size;
	int orechance = (csize * csize * csize) / clust_num_ores;
	int nclusters = volume / clust_scarcity;

	for (int i = 0; i != nclusters; i++) {
		int x0 = pr.range(nmin.X, nmax.X - csize + 1);
		int y0 = pr.range(nmin.Y, nmax.Y - csize + 1);
		int z0 = pr.range(nmin.Z, nmax.Z - csize + 1);
 
		if (np && (NoisePerlin3D(np, x0, y0, z0, seed) < nthresh))
			continue;

		for (int z1 = 0; z1 != csize; z1++)
		for (int y1 = 0; y1 != csize; y1++)
		for (int x1 = 0; x1 != csize; x1++) {
			if (pr.range(1, orechance) != 1)
				continue;

			u32 i = vm->m_area.index(x0 + x1, y0 + y1, z0 + z1);
			if (vm->m_data[i].getContent() == wherein)
				vm->m_data[i] = n_ore;
		}
	}
}


void OreSheet::generate(ManualMapVoxelManipulator *vm, int seed,
						u32 blockseed, v3s16 nmin, v3s16 nmax) {
	PseudoRandom pr(blockseed + 4234);
	MapNode n_ore(ore, 0, ore_param2);

	int max_height = clust_size;
	int y_start = pr.range(nmin.Y, nmax.Y - max_height);

	if (!noise) {
		int sx = nmax.X - nmin.X + 1;
		int sz = nmax.Z - nmin.Z + 1;
		noise = new Noise(np, 0, sx, sz);
	}
	noise->seed = seed + y_start;
	noise->perlinMap2D(nmin.X, nmin.Z);

	int index = 0;
	for (int z = nmin.Z; z <= nmax.Z; z++)
	for (int x = nmin.X; x <= nmax.X; x++) {
		float noiseval = noise->result[index++];
		if (noiseval < nthresh)
			continue;

		int height = max_height * (1. / pr.range(1, 3));
		int y0 = y_start + np->scale * noiseval; //pr.range(1, 3) - 1;
		int y1 = y0 + height;
		for (int y = y0; y != y1; y++) {
			u32 i = vm->m_area.index(x, y, z);
			if (!vm->m_area.contains(i))
				continue;

			if (vm->m_data[i].getContent() == wherein)
				vm->m_data[i] = n_ore;
		}
	}
}


///////////////////////////////////////////////////////////////////////////////


Decoration *createDecoration(DecorationType type) {
	switch (type) {
		case DECO_SIMPLE:
			return new DecoSimple;
		case DECO_SCHEMATIC:
			return new DecoSchematic;
		//case DECO_LSYSTEM:
		//	return new DecoLSystem;
		default:
			return NULL;
	}
}


Decoration::Decoration() {
	mapseed    = 0;
	np         = NULL;
	fill_ratio = 0;
	sidelen    = 1;
}


Decoration::~Decoration() {
	delete np;
}


void Decoration::resolveNodeNames(INodeDefManager *ndef) {
	if (c_place_on == CONTENT_IGNORE)
		c_place_on = ndef->getId(place_on_name);
}


void Decoration::placeDeco(Mapgen *mg, u32 blockseed, v3s16 nmin, v3s16 nmax) {
	PseudoRandom ps(blockseed + 53);
	int carea_size = nmax.X - nmin.X + 1;

	// Divide area into parts
	if (carea_size % sidelen) {
		errorstream << "Decoration::placeDeco: chunk size is not divisible by "
			"sidelen; setting sidelen to " << carea_size << std::endl;
		sidelen = carea_size;
	}
	
	s16 divlen = carea_size / sidelen;
	int area = sidelen * sidelen;

	for (s16 z0 = 0; z0 < divlen; z0++)
	for (s16 x0 = 0; x0 < divlen; x0++) {
		v2s16 p2d_center( // Center position of part of division
			nmin.X + sidelen / 2 + sidelen * x0,
			nmin.Z + sidelen / 2 + sidelen * z0
		);
		v2s16 p2d_min( // Minimum edge of part of division
			nmin.X + sidelen * x0,
			nmin.Z + sidelen * z0
		);
		v2s16 p2d_max( // Maximum edge of part of division
			nmin.X + sidelen + sidelen * x0 - 1,
			nmin.Z + sidelen + sidelen * z0 - 1
		);

		// Amount of decorations
		float nval = np ?
			NoisePerlin2D(np, p2d_center.X, p2d_center.Y, mapseed) :
			fill_ratio;
		u32 deco_count = area * MYMAX(nval, 0.f);

		for (u32 i = 0; i < deco_count; i++) {
			s16 x = ps.range(p2d_min.X, p2d_max.X);
			s16 z = ps.range(p2d_min.Y, p2d_max.Y);

			int mapindex = carea_size * (z - nmin.Z) + (x - nmin.X);
			
			s16 y = mg->heightmap ? 
					mg->heightmap[mapindex] :
					mg->findGroundLevel(v2s16(x, z), nmin.Y, nmax.Y);
					
			if (y < nmin.Y || y > nmax.Y)
				continue;

			int height = getHeight();
			int max_y = nmax.Y + MAP_BLOCKSIZE;
			if (y + 1 + height > max_y) {
				continue;
#if 0
				printf("Decoration at (%d %d %d) cut off\n", x, y, z);
				//add to queue
				JMutexAutoLock cutofflock(cutoff_mutex);
				cutoffs.push_back(CutoffData(x, y, z, height));
#endif
			}

			if (mg->biomemap) {
				std::set<u8>::iterator iter;
				
				if (biomes.size()) {
					iter = biomes.find(mg->biomemap[mapindex]);
					if (iter == biomes.end())
						continue;
				}
			}

			generate(mg, &ps, max_y, v3s16(x, y, z));
		}
	}
}


#if 0
void Decoration::placeCutoffs(Mapgen *mg, u32 blockseed, v3s16 nmin, v3s16 nmax) {
	PseudoRandom pr(blockseed + 53);
	std::vector<CutoffData> handled_cutoffs;
	
	// Copy over the cutoffs we're interested in so we don't needlessly hold a lock
	{
		JMutexAutoLock cutofflock(cutoff_mutex);
		for (std::list<CutoffData>::iterator i = cutoffs.begin();
			i != cutoffs.end(); ++i) {
			CutoffData cutoff = *i;
			v3s16 p    = cutoff.p;
			s16 height = cutoff.height;
			if (p.X < nmin.X || p.X > nmax.X ||
				p.Z < nmin.Z || p.Z > nmax.Z)
				continue;
			if (p.Y + height < nmin.Y || p.Y > nmax.Y)
				continue;
			
			handled_cutoffs.push_back(cutoff);
		}
	}
	
	// Generate the cutoffs
	for (size_t i = 0; i != handled_cutoffs.size(); i++) {
		v3s16 p    = handled_cutoffs[i].p;
		s16 height = handled_cutoffs[i].height;
		
		if (p.Y + height > nmax.Y) {
			//printf("Decoration at (%d %d %d) cut off again!\n", p.X, p.Y, p.Z);
			cuttoffs.push_back(v3s16(p.X, p.Y, p.Z));
		}
		
		generate(mg, &pr, nmax.Y, nmin.Y - p.Y, v3s16(p.X, nmin.Y, p.Z));
	}
	
	// Remove cutoffs that were handled from the cutoff list
	{
		JMutexAutoLock cutofflock(cutoff_mutex);
		for (std::list<CutoffData>::iterator i = cutoffs.begin();
			i != cutoffs.end(); ++i) {
			
			for (size_t j = 0; j != handled_cutoffs.size(); j++) {
				CutoffData coff = *i;
				if (coff.p == handled_cutoffs[j].p)
					i = cutoffs.erase(i);
			}
		}
	}	
}
#endif


///////////////////////////////////////////////////////////////////////////////


void DecoSimple::resolveNodeNames(INodeDefManager *ndef) {
	Decoration::resolveNodeNames(ndef);
	
	if (c_deco == CONTENT_IGNORE) {
		c_deco = ndef->getId(deco_name);
		if (c_deco == CONTENT_IGNORE) {
			errorstream << "DecoSimple::resolveNodeNames: decoration node '"
				<< deco_name << "' not defined" << std::endl;
			c_deco = CONTENT_AIR;
		}
	}
	if (c_spawnby == CONTENT_IGNORE) {
		c_spawnby = ndef->getId(spawnby_name);
		if (c_spawnby == CONTENT_IGNORE) {
			errorstream << "DecoSimple::resolveNodeNames: spawnby node '"
				<< deco_name << "' not defined" << std::endl;
			nspawnby = -1;
			c_spawnby = CONTENT_AIR;
		}
	}
	
	if (c_decolist.size())
		return;
	
	for (size_t i = 0; i != decolist_names.size(); i++) {		
		content_t c = ndef->getId(decolist_names[i]);
		if (c == CONTENT_IGNORE) {
			errorstream << "DecoSimple::resolveNodeNames: decolist node '"
				<< decolist_names[i] << "' not defined" << std::endl;
			c = CONTENT_AIR;
		}
		c_decolist.push_back(c);
	}
}


void DecoSimple::generate(Mapgen *mg, PseudoRandom *pr, s16 max_y, v3s16 p) {
	ManualMapVoxelManipulator *vm = mg->vm;

	u32 vi = vm->m_area.index(p);
	if (vm->m_data[vi].getContent() != c_place_on &&
		c_place_on != CONTENT_IGNORE)
		return;
		
	if (nspawnby != -1) {
		int nneighs = 0;
		v3s16 dirs[8] = { // a Moore neighborhood
			v3s16( 0, 0,  1),
			v3s16( 0, 0, -1),
			v3s16( 1, 0,  0),
			v3s16(-1, 0,  0),
			v3s16( 1, 0,  1),
			v3s16(-1, 0,  1),
			v3s16(-1, 0, -1),
			v3s16( 1, 0, -1)
		};
		
		for (int i = 0; i != 8; i++) {
			u32 index = vm->m_area.index(p + dirs[i]);
			if (vm->m_area.contains(index) &&
				vm->m_data[index].getContent() == c_spawnby)
				nneighs++;
		}
		
		if (nneighs < nspawnby)
			return;
	}
	
	size_t ndecos = c_decolist.size();
	content_t c_place = ndecos ? c_decolist[pr->range(0, ndecos - 1)] : c_deco;

	s16 height = (deco_height_max > 0) ?
		pr->range(deco_height, deco_height_max) : deco_height;

	height = MYMIN(height, max_y - p.Y);
	
	v3s16 em = vm->m_area.getExtent();
	for (int i = 0; i < height; i++) {
		vm->m_area.add_y(em, vi, 1);
		
		content_t c = vm->m_data[vi].getContent();
		if (c != CONTENT_AIR && c != CONTENT_IGNORE)
			break;
		
		vm->m_data[vi] = MapNode(c_place);
	}
}


int DecoSimple::getHeight() {
	return (deco_height_max > 0) ? deco_height_max : deco_height;
}


std::string DecoSimple::getName() {
	return deco_name;
}


///////////////////////////////////////////////////////////////////////////////


DecoSchematic::DecoSchematic() {
	node_names = NULL;
	schematic  = NULL;
	flags      = 0;
	size       = v3s16(0, 0, 0);
}


DecoSchematic::~DecoSchematic() {
	delete node_names;
	delete []schematic;
}


void DecoSchematic::resolveNodeNames(INodeDefManager *ndef) {
	Decoration::resolveNodeNames(ndef);
	
	if (filename.empty())
		return;
	
	if (!node_names) {
		errorstream << "DecoSchematic::resolveNodeNames: node name list was "
			"not created" << std::endl;
		return;
	}
	
	for (size_t i = 0; i != node_names->size(); i++) {
		content_t c = ndef->getId(node_names->at(i));
		if (c == CONTENT_IGNORE) {
			errorstream << "DecoSchematic::resolveNodeNames: node '"
				<< node_names->at(i) << "' not defined" << std::endl;
			c = CONTENT_AIR;
		}
		c_nodes.push_back(c);
	}
		
	for (int i = 0; i != size.X * size.Y * size.Z; i++)
		schematic[i].setContent(c_nodes[schematic[i].getContent()]);
	
	delete node_names;
	node_names = NULL;
}


void DecoSchematic::generate(Mapgen *mg, PseudoRandom *pr, s16 max_y, v3s16 p) {
	ManualMapVoxelManipulator *vm = mg->vm;

	if (flags & DECO_PLACE_CENTER_X)
		p.X -= (size.X + 1) / 2;
	if (flags & DECO_PLACE_CENTER_Y)
		p.Y -= (size.Y + 1) / 2;
	if (flags & DECO_PLACE_CENTER_Z)
		p.Z -= (size.Z + 1) / 2;
		
	u32 vi = vm->m_area.index(p);
	if (vm->m_data[vi].getContent() != c_place_on &&
		c_place_on != CONTENT_IGNORE)
		return;
	
	u32 i = 0;
	for (s16 z = 0; z != size.Z; z++)
	for (s16 y = 0; y != size.Y; y++) {
		vi = vm->m_area.index(p.X, p.Y + y, p.Z + z);
		for (s16 x = 0; x != size.X; x++, i++, vi++) {
			if (!vm->m_area.contains(vi))
				continue;
				
			content_t c = vm->m_data[vi].getContent();
			if (c != CONTENT_AIR && c != CONTENT_IGNORE)
				continue;
				
			if (schematic[i].param1 && myrand_range(1, 256) > schematic[i].param1)
				continue;
			
			vm->m_data[vi] = schematic[i];
			vm->m_data[vi].param1 = 0;
		}
	}
}


int DecoSchematic::getHeight() {
	return size.Y;
}


std::string DecoSchematic::getName() {
	return filename;
}


void DecoSchematic::placeStructure(Map *map, v3s16 p) {
	assert(schematic != NULL);
	ManualMapVoxelManipulator *vm = new ManualMapVoxelManipulator(map);

	if (flags & DECO_PLACE_CENTER_X)
		p.X -= (size.X + 1) / 2;
	if (flags & DECO_PLACE_CENTER_Y)
		p.Y -= (size.Y + 1) / 2;
	if (flags & DECO_PLACE_CENTER_Z)
		p.Z -= (size.Z + 1) / 2;
		
	v3s16 bp1 = getNodeBlockPos(p);
	v3s16 bp2 = getNodeBlockPos(p + size - v3s16(1,1,1));
	vm->initialEmerge(bp1, bp2);

	u32 i = 0;
	for (s16 z = 0; z != size.Z; z++)
	for (s16 y = 0; y != size.Y; y++) {
		u32 vi = vm->m_area.index(p.X, p.Y + y, p.Z + z);
		for (s16 x = 0; x != size.X; x++, i++, vi++) {
			if (!vm->m_area.contains(vi))
				continue;
			if (schematic[i].param1 && myrand_range(1, 256) > schematic[i].param1)
				continue;
			
			vm->m_data[vi] = schematic[i];
			vm->m_data[vi].param1 = 0;
		}
	}
	
	std::map<v3s16, MapBlock *> lighting_modified_blocks;
	std::map<v3s16, MapBlock *> modified_blocks;
	vm->blitBackAll(&modified_blocks);
	
	// TODO: Optimize this by using Mapgen::calcLighting() instead
	lighting_modified_blocks.insert(modified_blocks.begin(), modified_blocks.end());
	map->updateLighting(lighting_modified_blocks, modified_blocks);

	MapEditEvent event;
	event.type = MEET_OTHER;
	for (std::map<v3s16, MapBlock *>::iterator
		it = modified_blocks.begin();
		it != modified_blocks.end(); ++it)
		event.modified_blocks.insert(it->first);
		
	map->dispatchEvent(&event);
}


bool DecoSchematic::loadSchematicFile() {
	std::ifstream is(filename.c_str(), std::ios_base::binary);

	u32 signature = readU32(is);
	if (signature != 'MTSM') {
		errorstream << "loadSchematicFile: invalid schematic "
			"file" << std::endl;
		return false;
	}
	
	u16 version = readU16(is);
	if (version != 1) {
		errorstream << "loadSchematicFile: unsupported schematic "
			"file version" << std::endl;
		return false;
	}

	size = readV3S16(is);
	int nodecount = size.X * size.Y * size.Z;
	
	u16 nidmapcount = readU16(is);
	
	node_names = new std::vector<std::string>;
	for (int i = 0; i != nidmapcount; i++) {
		std::string name = deSerializeString(is);
		node_names->push_back(name);
	}

	delete schematic;
	schematic = new MapNode[nodecount];
	MapNode::deSerializeBulk(is, SER_FMT_VER_HIGHEST, schematic,
				nodecount, 2, 2, true);
				
	return true;
}


/*
	Minetest Schematic File Format

	All values are stored in big-endian byte order.
	[u32] signature: 'MTSM'
	[u16] version: 1
	[u16] size X
	[u16] size Y
	[u16] size Z
	[Name-ID table] Name ID Mapping Table
		[u16] name-id count
		For each name-id mapping:
			[u16] name length
			[u8[]] name
	ZLib deflated {
	For each node in schematic:  (for z, y, x)
		[u16] content
	For each node in schematic:
		[u8] probability of occurance (param1)
	For each node in schematic:
		[u8] param2
	}
*/
void DecoSchematic::saveSchematicFile(INodeDefManager *ndef) {
	std::ofstream os(filename.c_str(), std::ios_base::binary);

	writeU32(os, 'MTSM'); // signature
	writeU16(os, 1);      // version
	writeV3S16(os, size); // schematic size
	
	std::vector<content_t> usednodes;
	int nodecount = size.X * size.Y * size.Z;
	build_nnlist_and_update_ids(schematic, nodecount, &usednodes);
	
	u16 numids = usednodes.size();
	writeU16(os, numids); // name count
	for (int i = 0; i != numids; i++)
		os << serializeString(ndef->get(usednodes[i]).name); // node names
		
	// compressed bulk node data
	MapNode::serializeBulk(os, SER_FMT_VER_HIGHEST, schematic,
				nodecount, 2, 2, true);
}


void build_nnlist_and_update_ids(MapNode *nodes, u32 nodecount,
						std::vector<content_t> *usednodes) {
	std::map<content_t, content_t> nodeidmap;
	content_t numids = 0;
	
	for (u32 i = 0; i != nodecount; i++) {
		content_t id;
		content_t c = nodes[i].getContent();

		std::map<content_t, content_t>::const_iterator it = nodeidmap.find(c);
		if (it == nodeidmap.end()) {
			id = numids;
			numids++;

			usednodes->push_back(c);
			nodeidmap.insert(std::make_pair(c, id));
		} else {
			id = it->second;
		}
		nodes[i].setContent(id);
	}
}


bool DecoSchematic::getSchematicFromMap(Map *map, v3s16 p1, v3s16 p2) {
	ManualMapVoxelManipulator *vm = new ManualMapVoxelManipulator(map);

	v3s16 bp1 = getNodeBlockPos(p1);
	v3s16 bp2 = getNodeBlockPos(p2);
	vm->initialEmerge(bp1, bp2);
	
	size = p2 - p1 + 1;
	schematic = new MapNode[size.X * size.Y * size.Z];
	
	u32 i = 0;
	for (s16 z = p1.Z; z <= p2.Z; z++)
	for (s16 y = p1.Y; y <= p2.Y; y++) {
		u32 vi = vm->m_area.index(p1.X, y, z);
		for (s16 x = p1.X; x <= p2.X; x++, i++, vi++) {
			schematic[i] = vm->m_data[vi];
			schematic[i].param1 = 0;
		}
	}

	delete vm;
	return true;
}


void DecoSchematic::applyProbabilities(std::vector<std::pair<v3s16, u8> > *plist, v3s16 p0) {
	for (size_t i = 0; i != plist->size(); i++) {
		v3s16 p = (*plist)[i].first - p0;
		int index = p.Z * (size.Y * size.X) + p.Y * size.X + p.X;
		if (index < size.Z * size.Y * size.X)
			schematic[index].param1 = (*plist)[i].second;
	}
}


///////////////////////////////////////////////////////////////////////////////


Mapgen::Mapgen() {
	seed        = 0;
	water_level = 0;
	generating  = false;
	id          = -1;
	vm          = NULL;
	ndef        = NULL;
	heightmap   = NULL;
	biomemap    = NULL;
}


// Returns Y one under area minimum if not found
s16 Mapgen::findGroundLevelFull(v2s16 p2d) {
	v3s16 em = vm->m_area.getExtent();
	s16 y_nodes_max = vm->m_area.MaxEdge.Y;
	s16 y_nodes_min = vm->m_area.MinEdge.Y;
	u32 i = vm->m_area.index(p2d.X, y_nodes_max, p2d.Y);
	s16 y;
	
	for (y = y_nodes_max; y >= y_nodes_min; y--) {
		MapNode &n = vm->m_data[i];
		if (ndef->get(n).walkable)
			break;

		vm->m_area.add_y(em, i, -1);
	}
	return (y >= y_nodes_min) ? y : y_nodes_min - 1;
}


s16 Mapgen::findGroundLevel(v2s16 p2d, s16 ymin, s16 ymax) {
	v3s16 em = vm->m_area.getExtent();
	u32 i = vm->m_area.index(p2d.X, ymax, p2d.Y);
	s16 y;
	
	for (y = ymax; y >= ymin; y--) {
		MapNode &n = vm->m_data[i];
		if (ndef->get(n).walkable)
			break;

		vm->m_area.add_y(em, i, -1);
	}
	return y;
}


void Mapgen::updateHeightmap(v3s16 nmin, v3s16 nmax) {
	if (!heightmap)
		return;
	
	//TimeTaker t("Mapgen::updateHeightmap", NULL, PRECISION_MICRO);
	int index = 0;
	for (s16 z = nmin.Z; z <= nmax.Z; z++) {
		for (s16 x = nmin.X; x <= nmax.X; x++) {
			s16 y = findGroundLevel(v2s16(x, z), nmin.Y, nmax.Y);
			heightmap[index++] = y;
		}
	}
	//printf("updateHeightmap: %dus\n", t.stop());
}


void Mapgen::updateLiquid(UniqueQueue<v3s16> *trans_liquid, v3s16 nmin, v3s16 nmax) {
	bool isliquid, wasliquid;
	v3s16 em  = vm->m_area.getExtent();

	for (s16 z = nmin.Z; z <= nmax.Z; z++) {
		for (s16 x = nmin.X; x <= nmax.X; x++) {
			wasliquid = true;

			u32 i = vm->m_area.index(x, nmax.Y, z);
			for (s16 y = nmax.Y; y >= nmin.Y; y--) {
				isliquid = ndef->get(vm->m_data[i]).isLiquid();

				// there was a change between liquid and nonliquid, add to queue
				if (isliquid != wasliquid)
					trans_liquid->push_back(v3s16(x, y, z));

				wasliquid = isliquid;
				vm->m_area.add_y(em, i, -1);
			}
		}
	}
}


void Mapgen::setLighting(v3s16 nmin, v3s16 nmax, u8 light) {
	ScopeProfiler sp(g_profiler, "EmergeThread: mapgen lighting update", SPT_AVG);
	VoxelArea a(nmin, nmax);

	for (int z = a.MinEdge.Z; z <= a.MaxEdge.Z; z++) {
		for (int y = a.MinEdge.Y; y <= a.MaxEdge.Y; y++) {
			u32 i = vm->m_area.index(a.MinEdge.X, y, z);
			for (int x = a.MinEdge.X; x <= a.MaxEdge.X; x++, i++)
				vm->m_data[i].param1 = light;
		}
	}
}


void Mapgen::lightSpread(VoxelArea &a, v3s16 p, u8 light) {
	if (light <= 1 || !a.contains(p))
		return;

	u32 vi = vm->m_area.index(p);
	MapNode &nn = vm->m_data[vi];

	light--;
	// should probably compare masked, but doesn't seem to make a difference
	if (light <= nn.param1 || !ndef->get(nn).light_propagates)
		return;

	nn.param1 = light;

	lightSpread(a, p + v3s16(0, 0, 1), light);
	lightSpread(a, p + v3s16(0, 1, 0), light);
	lightSpread(a, p + v3s16(1, 0, 0), light);
	lightSpread(a, p - v3s16(0, 0, 1), light);
	lightSpread(a, p - v3s16(0, 1, 0), light);
	lightSpread(a, p - v3s16(1, 0, 0), light);
}


void Mapgen::calcLighting(v3s16 nmin, v3s16 nmax) {
	VoxelArea a(nmin, nmax);
	bool block_is_underground = (water_level >= nmax.Y);

	ScopeProfiler sp(g_profiler, "EmergeThread: mapgen lighting update", SPT_AVG);
	//TimeTaker t("updateLighting");

	// first, send vertical rays of sunshine downward
	v3s16 em = vm->m_area.getExtent();
	for (int z = a.MinEdge.Z; z <= a.MaxEdge.Z; z++) {
		for (int x = a.MinEdge.X; x <= a.MaxEdge.X; x++) {
			// see if we can get a light value from the overtop
			u32 i = vm->m_area.index(x, a.MaxEdge.Y + 1, z);
			if (vm->m_data[i].getContent() == CONTENT_IGNORE) {
				if (block_is_underground)
					continue;
			} else if ((vm->m_data[i].param1 & 0x0F) != LIGHT_SUN) {
				continue;
			}
			vm->m_area.add_y(em, i, -1);
 
			for (int y = a.MaxEdge.Y; y >= a.MinEdge.Y; y--) {
				MapNode &n = vm->m_data[i];
				if (!ndef->get(n).sunlight_propagates)
					break;
				n.param1 = LIGHT_SUN;
				vm->m_area.add_y(em, i, -1);
			}
		}
	}

	// now spread the sunlight and light up any sources
	for (int z = a.MinEdge.Z; z <= a.MaxEdge.Z; z++) {
		for (int y = a.MinEdge.Y; y <= a.MaxEdge.Y; y++) {
			u32 i = vm->m_area.index(a.MinEdge.X, y, z);
			for (int x = a.MinEdge.X; x <= a.MaxEdge.X; x++, i++) {
				MapNode &n = vm->m_data[i];
				if (n.getContent() == CONTENT_IGNORE ||
					!ndef->get(n).light_propagates)
					continue;

				u8 light_produced = ndef->get(n).light_source & 0x0F;
				if (light_produced)
					n.param1 = light_produced;

				u8 light = n.param1 & 0x0F;
				if (light) {
					lightSpread(a, v3s16(x,     y,     z + 1), light);
					lightSpread(a, v3s16(x,     y + 1, z    ), light);
					lightSpread(a, v3s16(x + 1, y,     z    ), light);
					lightSpread(a, v3s16(x,     y,     z - 1), light);
					lightSpread(a, v3s16(x,     y - 1, z    ), light);
					lightSpread(a, v3s16(x - 1, y,     z    ), light);
				}
			}
		}
	}

	//printf("updateLighting: %dms\n", t.stop());
}


void Mapgen::calcLightingOld(v3s16 nmin, v3s16 nmax) {
	enum LightBank banks[2] = {LIGHTBANK_DAY, LIGHTBANK_NIGHT};
	VoxelArea a(nmin, nmax);
	bool block_is_underground = (water_level > nmax.Y);
	bool sunlight = !block_is_underground;

	ScopeProfiler sp(g_profiler, "EmergeThread: mapgen lighting update", SPT_AVG);

	for (int i = 0; i < 2; i++) {
		enum LightBank bank = banks[i];
		std::set<v3s16> light_sources;
		std::map<v3s16, u8> unlight_from;

		voxalgo::clearLightAndCollectSources(*vm, a, bank, ndef,
											 light_sources, unlight_from);
		voxalgo::propagateSunlight(*vm, a, sunlight, light_sources, ndef);

		vm->unspreadLight(bank, unlight_from, light_sources, ndef);
		vm->spreadLight(bank, light_sources, ndef);
	}
}
 
 
//////////////////////// Mapgen V6 parameter read/write
 
bool MapgenV6Params::readParams(Settings *settings) {
	freq_desert = settings->getFloat("mgv6_freq_desert");
	freq_beach  = settings->getFloat("mgv6_freq_beach");

	bool success = 
		settings->getNoiseParams("mgv6_np_terrain_base",   np_terrain_base)   &&
		settings->getNoiseParams("mgv6_np_terrain_higher", np_terrain_higher) &&
		settings->getNoiseParams("mgv6_np_steepness",      np_steepness)      &&
		settings->getNoiseParams("mgv6_np_height_select",  np_height_select)  &&
		settings->getNoiseParams("mgv6_np_mud",            np_mud)            &&
		settings->getNoiseParams("mgv6_np_beach",          np_beach)          &&
		settings->getNoiseParams("mgv6_np_biome",          np_biome)          &&
		settings->getNoiseParams("mgv6_np_cave",           np_cave)           &&
		settings->getNoiseParams("mgv6_np_humidity",       np_humidity)       &&
		settings->getNoiseParams("mgv6_np_trees",          np_trees)          &&
		settings->getNoiseParams("mgv6_np_apple_trees",    np_apple_trees);
	return success;
}
 
 
void MapgenV6Params::writeParams(Settings *settings) {
	settings->setFloat("mgv6_freq_desert", freq_desert);
	settings->setFloat("mgv6_freq_beach",  freq_beach);
 
	settings->setNoiseParams("mgv6_np_terrain_base",   np_terrain_base);
	settings->setNoiseParams("mgv6_np_terrain_higher", np_terrain_higher);
	settings->setNoiseParams("mgv6_np_steepness",      np_steepness);
	settings->setNoiseParams("mgv6_np_height_select",  np_height_select);
	settings->setNoiseParams("mgv6_np_mud",            np_mud);
	settings->setNoiseParams("mgv6_np_beach",          np_beach);
	settings->setNoiseParams("mgv6_np_biome",          np_biome);
	settings->setNoiseParams("mgv6_np_cave",           np_cave);
	settings->setNoiseParams("mgv6_np_humidity",       np_humidity);
	settings->setNoiseParams("mgv6_np_trees",          np_trees);
	settings->setNoiseParams("mgv6_np_apple_trees",    np_apple_trees);
}


bool MapgenV7Params::readParams(Settings *settings) {
	bool success = 
		settings->getNoiseParams("mgv7_np_terrain_base",    np_terrain_base)    &&
		settings->getNoiseParams("mgv7_np_terrain_alt",     np_terrain_alt)     &&
		settings->getNoiseParams("mgv7_np_terrain_mod",     np_terrain_mod)     &&
		settings->getNoiseParams("mgv7_np_terrain_persist", np_terrain_persist) &&
		settings->getNoiseParams("mgv7_np_height_select",   np_height_select)   &&
		settings->getNoiseParams("mgv7_np_ridge",           np_ridge);
	return success;
}


void MapgenV7Params::writeParams(Settings *settings) {
	settings->setNoiseParams("mgv7_np_terrain_base",    np_terrain_base);
	settings->setNoiseParams("mgv7_np_terrain_alt",     np_terrain_alt);
	settings->setNoiseParams("mgv7_np_terrain_mod",     np_terrain_mod);
	settings->setNoiseParams("mgv7_np_terrain_persist", np_terrain_persist);
	settings->setNoiseParams("mgv7_np_height_select",   np_height_select);
	settings->setNoiseParams("mgv7_np_ridge",           np_ridge);
}


/////////////////////////////////// legacy static functions for farmesh

s16 Mapgen::find_ground_level_from_noise(u64 seed, v2s16 p2d, s16 precision) {
	//just need to return something
	s16 level = 5;
	return level;
}


bool Mapgen::get_have_beach(u64 seed, v2s16 p2d) {
	double sandnoise = noise2d_perlin(
				0.2+(float)p2d.X/250, 0.7+(float)p2d.Y/250,
				seed+59420, 3, 0.50);
 
	return (sandnoise > 0.15);
}


double Mapgen::tree_amount_2d(u64 seed, v2s16 p) {
	double noise = noise2d_perlin(
			0.5+(float)p.X/125, 0.5+(float)p.Y/125,
			seed+2, 4, 0.66);
	double zeroval = -0.39;
	if(noise < zeroval)
		return 0;
	else
		return 0.04 * (noise-zeroval) / (1.0-zeroval);
}
