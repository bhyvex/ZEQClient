
#include "wld.h"

extern Renderer gRenderer;

WLD::WLD(MemoryStream* mem, S3D* s3d, std::string shortname) :
	mShortName(shortname),
	mContainingS3D(s3d),
	mNumMaterials(0),
	mMaterials(nullptr),
	mMaterialVertexBuffers(nullptr),
	mMaterialIndexBuffers(nullptr)
{
	byte* data = mem->getData();

	mHeader = (Header*)data;
	uint32 p = sizeof(Header);

	const char* m = mHeader->magic;
	if (m[0] != 0x02 || m[1] != 0x3D || m[2] != 0x50 || m[3] != 0x54)
		throw ZEQException("WLD::WLD: File was not a valid WLD");

	mHeader->version &= 0xFFFFFFFE;
	if (mHeader->version != Header::VERSION1 && mHeader->version != Header::VERSION2)
		throw ZEQException("WLD::WLD: File was not a valid WLD version");
	mVersion = (mHeader->version == Header::VERSION1) ? 1 : 2;

	char* strings = (char*)&data[p];
	decodeString(strings, mHeader->strings_len);
	mStringBlock = strings;
	p += mHeader->strings_len;

	//zeroth fragment is null - saves us from doing a lot of 'ref - 1' later on
	mFragsByIndex.push_back(nullptr);

	const uint32 count = mHeader->frag_count;
	for (uint32 i = 0; i < count; ++i)
	{
		FragHeader* fh = (FragHeader*)&data[p];

		mFragsByIndex.push_back(fh);
		mFragsByType[fh->type].push_back(fh);
		if (fh->nameref < 0)
			mFragsByNameRef[-fh->nameref] = fh;

		p += FragHeader::SIZE + fh->len;
	}
}

WLD::~WLD()
{
	if (mMaterials)
		delete[] mMaterials;
	if (mMaterialVertexBuffers)
	{
		for (uint32 i = 0; i < mNumMaterials; ++i)
			mMaterialVertexBuffers[i].~vector();
		delete[] mMaterialVertexBuffers;
	}
	if (mMaterialIndexBuffers)
	{
		for (uint32 i = 0; i < mNumMaterials; ++i)
			mMaterialIndexBuffers[i].~vector();
		delete[] mMaterialIndexBuffers;
	}
}

void WLD::decodeString(void* str, size_t len)
{
	static byte hashval[] = {0x95, 0x3A, 0xC5, 0x2A, 0x95, 0x7A, 0x95, 0x6A};
	byte* data = (byte*)str;
	for (size_t i = 0; i < len; ++i)
	{
		data[i] ^= hashval[i & 7];
	}
}

FragHeader* WLD::getFragByRef(int ref)
{
	if (ref > 0)
	{
		return mFragsByIndex[ref];
	}
	else
	{
		if (ref == 0)
			ref = -1;
		if (mFragsByNameRef.count(ref))
			return mFragsByNameRef[ref];
	}

	return nullptr;
}

const char* WLD::getFragName(FragHeader* frag)
{
	return getFragName(frag->nameref);
}

const char* WLD::getFragName(int ref)
{
	uint32 r = -ref;
	if (ref >= 0 || r >= mHeader->strings_len)
		return nullptr;
	return mStringBlock - ref;
}

void WLD::processMaterials()
{
	if (mMaterials || mFragsByType.count(0x03) == 0)
		return;

	//pre-process 0x03 frags so their strings are only decoded once each
	for (FragHeader* frag : mFragsByType[0x03])
	{
		Frag03* f03 = (Frag03*)frag;
		if (f03->string_len == 0)
			continue;

		decodeString(f03->string, f03->string_len);
		Util::toLower((char*)f03->string, f03->string_len);
		mTexturesByFrag03[f03] = (const char*)f03->string;
	}

	//process 0x30 frags to find all materials in the wld
	if (mFragsByType.count(0x30) == 0)
		return;

	mNumMaterials = mFragsByType[0x30].size();

	mMaterials = new IntermediateMaterial[mNumMaterials];
	for (uint32 i = 0; i < mNumMaterials; ++i)
		new (&mMaterials[i]) IntermediateMaterial;

	int i = -1;
	for (FragHeader* frag : mFragsByType[0x30])
	{
		Frag30* f30 = (Frag30*)frag;
		Frag03* f03;

		IntermediateMaterial* mat = &mMaterials[++i];

		mMaterialIndicesByFrag30[f30] = i;
		
		//f30 -> f05 -> f04 -> f03
		//OR
		//f30 -> f03 (may have null texture)
		if (f30->ref > 0)
		{
			Frag05* f05 = (Frag05*)getFragByRef(f30->ref);
			if (f05 == nullptr)
				continue;
			Frag04* f04 = (Frag04*)getFragByRef(f05->ref);
			if (f04 == nullptr)
				continue;
			//we have our f04, check if it's animated
			if (!f04->isAnimated())
			{
				f03 = (Frag03*)getFragByRef(f04->ref);
			}
			else
			{
				handleAnimatedMaterial(f04, f30, mat);
				continue;
			}
		}
		else
		{
			f03 = (Frag03*)getFragByRef(f30->ref);
		}

		//we have our f03 frag, time to make our material
		Frag03ToMaterialEntry(f03, f30, &mat->first);
	}
}

void WLD::Frag03ToMaterialEntry(Frag03* f03, Frag30* f30, IntermediateMaterialEntry* mat_ent)
{
	if (f03 && f03->string_len > 0)
	{
		//we have a texture name, create it in the renderer
		MemoryStream* file = mContainingS3D->getFile((char*)f03->string);
		if (file == nullptr)
			return;
		std::string name = mShortName;
		name += '/';
		name += getFragName((FragHeader*)f30);
		bool isDDS = false;

		mat_ent->diffuse_map = gRenderer.createTexture(file, name, isDDS);

		mat_ent->flag = translateVisibilityFlag(f30, isDDS);
	}
	else
	{
		//null material
		mat_ent->flag = IntermediateMaterialEntry::FULLY_TRANSPARENT;
	}
}

void WLD::handleAnimatedMaterial(Frag04* f04, Frag30* f30, IntermediateMaterial* mat)
{
	Frag04Animated* f04a = (Frag04Animated*)f04;

	mat->num_frames = f04a->count;
	mat->frame_delay = f04a->milliseconds;
	int add = f04a->count - 1;
	mat->additional = new IntermediateMaterialEntry[add];
	for (int i = 0; i < add; ++i)
		new (&mat->additional[i]) IntermediateMaterialEntry;

	int* ref_ptr = f04a->getRefList();

	//first is outside the array
	IntermediateMaterialEntry* mat_ent = &mat->first;
	Frag03* f03 = (Frag03*)getFragByRef(*ref_ptr++);
	Frag03ToMaterialEntry(f03, f30, mat_ent);

	for (int i = 0; i < add; ++i)
	{
		mat_ent = &mat->additional[i];
		f03 = (Frag03*)getFragByRef(*ref_ptr++);
		Frag03ToMaterialEntry(f03, f30, mat_ent);
	}
}

uint32 WLD::translateVisibilityFlag(Frag30* f30, bool isDDS)
{
	uint32 ret = 0;

	if (f30->visibility_flag == 0)
		return IntermediateMaterialEntry::FULLY_TRANSPARENT;
	if ((f30->visibility_flag & Frag30::MASKED) == Frag30::MASKED || (f30->visibility_flag & 0xB) == 0xB)
		ret |= IntermediateMaterialEntry::MASKED;
	if ((f30->visibility_flag & Frag30::SEMI_TRANSPARENT) == Frag30::SEMI_TRANSPARENT)
		ret |= IntermediateMaterialEntry::SEMI_TRANSPARENT;
	if (isDDS)
		ret |= IntermediateMaterialEntry::DDS_TEXTURE;

	return ret;
}

void WLD::processMesh(Frag36* f36)
{
	byte* data = (byte*)f36;
	uint32 p = sizeof(Frag36);

	const float scale = 1.0f / (1 << f36->scale);

	//raw vertices
	RawVertex* wld_verts = (RawVertex*)(data + p);
	p += sizeof(RawVertex) * f36->vert_count;

	//raw uvs
	RawUV16* uv16 = nullptr;
	RawUV32* uv32 = nullptr;
	if (f36->uv_count > 0)
	{
		if (getVersion() == 1)
		{
			uv16 = (RawUV16*)(data + p);
			p += sizeof(RawUV16) * f36->uv_count;
		}
		else
		{
			uv32 = (RawUV32*)(data + p);
			p += sizeof(RawUV32) * f36->uv_count;
		}
	}

	//raw normals
	RawNormal* wld_norm = (RawNormal*)(data + p);
	p += sizeof(RawNormal) * f36->vert_count;

	//skip vertex colors (for now?)
	p += sizeof(uint32) * f36->color_count;

	//raw triangles
	RawTriangle* wld_tris = (RawTriangle*)(data + p);
	p += sizeof(RawTriangle) * f36->poly_count;

	//skip bone assignments
	p += sizeof(BoneAssignment) * f36->bone_assignment_count;

	//get material indices for triangles
	std::unordered_map<uint32, int> mat_index_list;
	Frag31* f31 = (Frag31*)getFragByRef(f36->texture_list_ref);
	int* ref_ptr = f31->getRefList();
	for (uint32 i = 0; i < f31->ref_count; ++i)
	{
		Frag30* f30 = (Frag30*)getFragByRef(*ref_ptr++);
		mat_index_list[i] = mMaterialIndicesByFrag30[f30];
	}

	auto processTriangle = [=](RawTriangle& tri, std::vector<video::S3DVertex>& vert_buf, std::vector<uint32>& index_buf)
	{
		uint32 base = vert_buf.size();

		//handle uv conversions
		video::S3DVertex vertex;
		if (uv16)
		{
			for (int i = 0; i < 3; ++i)
			{
				static const float uv_scale = 1.0f / 256.0f;
				RawUV16& uv = uv16[tri.index[i]];
				vertex.TCoords.X = (float)uv.u * uv_scale;
				vertex.TCoords.Y = -((float)uv.v * uv_scale);
				vert_buf.push_back(vertex);
			}
		}
		else if (uv32)
		{
			for (int i = 0; i < 3; ++i)
			{
				RawUV32& uv = uv32[tri.index[i]];
				vertex.TCoords.X = uv.u;
				vertex.TCoords.Y = -uv.v;
				vert_buf.push_back(vertex);
			}
		}
		else
		{
			vertex.TCoords.X = 0;
			vertex.TCoords.Y = 0;
			for (int i = 0; i < 3; ++i)
				vert_buf.push_back(vertex);
		}

		//handle vertex and normal conversions
		uint32 buf_pos = base;
		for (int i = 0; i < 3; ++i)
		{
			video::S3DVertex& vertex = vert_buf[buf_pos++];
			uint16 idx = tri.index[i];
			RawVertex& vert = wld_verts[idx];
			vertex.Pos.X = f36->x + (float)vert.x * scale;
			vertex.Pos.Z = f36->y + (float)vert.y * scale; //irrlicht uses Y for the "up" axis, need to switch
			vertex.Pos.Y = f36->z + (float)vert.z * scale;
			RawNormal& norm = wld_norm[idx];
			static const float normal_scale = 1.0f / 127.0f;
			vertex.Normal.X = (float)norm.i * normal_scale;
			vertex.Normal.Z = (float)norm.j * normal_scale;
			vertex.Normal.Y = (float)norm.k * normal_scale;
		}

		//write indices (purely in order for now, no reused vertices)
		//would be faster to do them all per material in one step, in that case... but we should look at reusing vertices instead
		for (int i = 0; i < 3; ++i)
			index_buf.push_back(base++);
	};

	//construct vertices and triangles based on their materials
	for (uint16 m = 0; m < f36->poly_texture_count; ++m)
	{
		RawTextureEntry* rte = (RawTextureEntry*)(data + p);
		p += sizeof(RawTextureEntry);

		int mat_index = mat_index_list[rte->index];

		//get buffers
		std::vector<video::S3DVertex>& vert_buf = mMaterialVertexBuffers[mat_index];
		std::vector<uint32>& index_buf = mMaterialIndexBuffers[mat_index];
		std::vector<video::S3DVertex>& nocollide_vert_buf = mNoCollisionVertexBuffers[mat_index];
		std::vector<uint32>& nocollide_index_buf = mNoCollisionIndexBuffers[mat_index];

		for (uint16 i = 0; i < rte->count; ++i)
		{
			RawTriangle& tri = wld_tris[i];
			if ((tri.flag & RawTriangle::PERMEABLE) == 0)
				processTriangle(tri, vert_buf, index_buf);
			else
				processTriangle(tri, nocollide_vert_buf, nocollide_index_buf);
		}

		//advance triangles ptr for the next block
		wld_tris += rte->count;
	}
}

void WLD::initMaterialBuffers()
{
	if (mMaterialVertexBuffers)
		return; //already created
	mMaterialVertexBuffers = new std::vector<video::S3DVertex>[mNumMaterials];
	mMaterialIndexBuffers = new std::vector<uint32>[mNumMaterials];
	mNoCollisionVertexBuffers = new std::vector<video::S3DVertex>[mNumMaterials];
	mNoCollisionIndexBuffers = new std::vector<uint32>[mNumMaterials];
	for (uint32 i = 0; i < mNumMaterials; ++i)
	{
		//placement new
		new (&mMaterialVertexBuffers[i]) std::vector<video::S3DVertex>;
		new (&mMaterialIndexBuffers[i]) std::vector<uint32>;
		new (&mNoCollisionVertexBuffers[i]) std::vector<video::S3DVertex>;
		new (&mNoCollisionIndexBuffers[i]) std::vector<uint32>;
	}
}

ZoneModel* WLD::convertZoneGeometry()
{
	if (mFragsByType.count(0x36) == 0)
		return nullptr;

	processMaterials();
	
	//initialize two buffers for each material
	initMaterialBuffers();
	//should also make a collision mesh buffer here...
	
	//process mesh fragments
	for (FragHeader* frag : mFragsByType[0x36])
	{
		processMesh((Frag36*)frag);
	}

	//create the irrlicht mesh, transferring buffers and creating final materials
	scene::SMesh* mesh = new scene::SMesh;
	scene::SMesh* nocollide_mesh = new scene::SMesh;
	ZoneModel* zone = new ZoneModel;

	for (uint32 i = 0; i < mNumMaterials; ++i)
	{
		if (!mMaterialVertexBuffers[i].empty())
			createMeshBuffer(mesh, mMaterialVertexBuffers[i], mMaterialIndexBuffers[i], &mMaterials[i], zone);
		if (!mNoCollisionVertexBuffers[i].empty())
			createMeshBuffer(nocollide_mesh, mNoCollisionVertexBuffers[i], mNoCollisionIndexBuffers[i], &mMaterials[i], zone);
	}

	mesh->recalculateBoundingBox();
	nocollide_mesh->recalculateBoundingBox();
	zone->setMeshes(mesh, nocollide_mesh);

	return zone;
}

void WLD::convertZoneObjectDefinitions(ZoneModel* zone)
{
	if (mFragsByType.count(0x14) == 0)
		return;

	processMaterials();
	
	//initialize two buffers for each material
	initMaterialBuffers();

	//process mesh fragments one by one
	//0x14 -> 0x2D -> 0x36, 0x14 contains the name
	for (FragHeader* frag : mFragsByType[0x14])
	{
		Frag14* f14 = (Frag14*)frag;
		const char* model_name = getFragName(frag);
		if (f14->size[1] < 1 || model_name == nullptr)
			continue;

		int* ref_ptr = f14->getRefList();
		Frag2D* f2d = (Frag2D*)getFragByRef(*ref_ptr);
		if (f2d == nullptr || f2d->type != 0x2D)
			continue;

		processMesh((Frag36*)getFragByRef(f2d->ref));

		//create irrlicht mesh
		scene::SMesh* mesh = new scene::SMesh;
		scene::SMesh* noncollision_mesh = new scene::SMesh;
		//find any vertex + index buffers with data in them and use them to fill this mesh
		for (uint32 i = 0; i < mNumMaterials; ++i)
		{
			if (!mMaterialVertexBuffers[i].empty())
			{
				createMeshBuffer(mesh, mMaterialVertexBuffers[i], mMaterialIndexBuffers[i], &mMaterials[i], zone);
				mMaterialVertexBuffers[i].clear();
				mMaterialIndexBuffers[i].clear();
			}
			if (!mNoCollisionVertexBuffers[i].empty())
			{
				createMeshBuffer(noncollision_mesh, mNoCollisionVertexBuffers[i], mNoCollisionIndexBuffers[i], &mMaterials[i], zone);
				mNoCollisionVertexBuffers[i].clear();
				mNoCollisionIndexBuffers[i].clear();
			}
		}

		if (mesh->getMeshBufferCount() > 0)
		{
			mesh->recalculateBoundingBox();
			zone->addObjectDefinition(model_name, mesh);
		}

		if (noncollision_mesh->getMeshBufferCount() > 0)
		{
			noncollision_mesh->recalculateBoundingBox();
			zone->addNoCollisionObjectDefinition(model_name, noncollision_mesh);
		}
	}
}

void WLD::convertZoneObjectPlacements(ZoneModel* zone)
{
	if (mFragsByType.count(0x15) == 0)
		return;

	for (FragHeader* frag : mFragsByType[0x15])
	{
		Frag15* f15 = (Frag15*)frag;
		const char* name = getFragName(f15->ref1);
		if (name == nullptr)
			continue;

		ObjectPlacement obj;
		obj.x = f15->pos.x;
		obj.y = f15->pos.z; //irrlicht uses Y for the "up" axis
		obj.z = f15->pos.y;

		obj.rotX = f15->rot.y / 512.0f * 360.0f;
		obj.rotY = -f15->rot.x / 512.0f * 360.0f; //don't even try to make sense of these
		obj.rotZ = f15->rot.z / 512.0f * 360.0f;

		obj.scaleX = f15->scale.z;
		obj.scaleY = f15->scale.y; //scale order is also weird - x is generally not given, assumed to be equal to the others
		obj.scaleZ = f15->scale.z;

		zone->addObjectPlacement(name, obj);
	}
}

void WLD::createMeshBuffer(scene::SMesh* mesh, std::vector<video::S3DVertex>& vert_buf,
		std::vector<uint32>& index_buf, IntermediateMaterial* mat, ZoneModel* zone)
{
	//irrlicht's default provided mesh buffers can take up to 65536 indices
	//need to handle case of total > 65535 by splitting into separate buffers
	uint32 count = index_buf.size();
	uint32 n = (count / 65535) + 1; //may want to make sure it's not an exact multiple
	uint32 adj = 0;

	AnimatedTexture* animTex = nullptr;
	if (mat->num_frames > 1 && zone)
	{
		AnimatedTexture anim(mesh, mat, n, mesh->getMeshBufferCount());
		animTex = zone->addAnimatedTexture(anim);
	}

	for (uint32 i = 0; i < n; ++i)
	{
		scene::SMeshBuffer* mesh_buffer = new scene::SMeshBuffer;
		auto& vbuf = mesh_buffer->Vertices;
		auto& ibuf = mesh_buffer->Indices;

		//65535 is a multiple of 3, use 0 to 65534
		uint32 max = count < 65535 ? count : 65535;

		for (uint32 j = 0; j < max; ++j)
		{
			vbuf.push_back(vert_buf[j + adj]);
			ibuf.push_back((uint16)(index_buf[j + adj] - adj));
		}

		//material
		video::SMaterial& material = mesh_buffer->getMaterial();
		if (mat)
		{
			if (mat->first.flag & IntermediateMaterialEntry::FULLY_TRANSPARENT)
			{
				material.MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL_REF; //fully transparent materials should have no texture
				//use TRANSPARENT_VERTEX_ALPHA to show zone walls
			}
			else if (mat->first.diffuse_map)
			{
				material.setTexture(0, mat->first.diffuse_map);
				//zone->addUsedTexture(mat->first.diffuse_map); //make a general Model class for this

				if (mat->first.flag & IntermediateMaterialEntry::MASKED)
					material.MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL_REF; //should be blended
				//put semi-transparent handling here (need to figure out how to do it and how to make it play nice with masking...)
				//probably make a copy of the texture and change the alpha of the bitmap pixels, then use blending EMT_TRANSPARENT_ALPHA_CHANNEL
			}
		}
		else
		{
			material.MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL_REF;
		}

		mesh_buffer->recalculateBoundingBox();
		mesh->addMeshBuffer(mesh_buffer);
		mesh_buffer->drop();

		adj += 65535;
		count -= 65535;
	}
}

MobModel* WLD::convertMobModel(const char* id_name)
{
	if (mFragsByType.count(0x14) == 0)
		return nullptr;

	processMaterials();

	size_t len = strlen(id_name);

	for (FragHeader* frag : mFragsByType[0x14])
	{
		const char* name = getFragName(frag);
		if (strncmp(id_name, name, len) == 0)
		{
			return convertMobModel((Frag14*)frag);
		}
	}

	return nullptr;
}

MobModel* WLD::convertMobModel(Frag14* f14)
{
	if (f14->size[1] < 1)
		return nullptr;

	//should handle this separately from zones in a lot of ways
	//should not load all material textures off the bat in case we are only using 1 out of 10 models in a WLD, for one
	
	//process bones to get all the joints in order (check for and record attachment points!)
	//then process the 0x36s to get the bone assignments
	//and maybe also do the 0x30 --> 0x03 part there too if you can avoid making a mess

	//should flag chr wld's and remove hardware textures on their destructors
	
	//initialize two buffers for each material
	initMaterialBuffers();

	//0x14 -> 0x11 -> 0x10 -> 0x13 -> 0x12
	//                   | -> 0x2D -> 0x36
	int* ref_ptr = f14->getRefList();
	Frag11* f11 = (Frag11*)getFragByRef(*ref_ptr);
	if (f11->type != 0x11)
		return nullptr;
	Frag10* f10 = (Frag10*)getFragByRef(f11->ref);

	MobModel* mob = new MobModel;

	//handle skeleton here
	Frag10Bone* bone = f10->getBoneList();
	//the skeleton uses index-based recursion, so let's facilitate that
	Frag10Bone* bone_ptr = bone;
	std::vector<Frag10Bone*> skeletonSet;
	for (int i = 0; i < f10->num_bones; ++i)
	{
		skeletonSet.push_back(bone_ptr);
		bone_ptr = bone_ptr->getNext();
	}

	//find meshes
	int numMeshes;
	ref_ptr = f10->getRefList(numMeshes);
	for (int n = 0; n < numMeshes; ++n)
	{
		Frag2D* f2d = (Frag2D*)getFragByRef(*ref_ptr++);

		processMesh((Frag36*)getFragByRef(f2d->ref));

		//create irrlicht mesh
		scene::SMesh* mesh = new scene::SMesh;
		//find any vertex + index buffers with data in them and use them to fill this mesh
		for (uint32 i = 0; i < mNumMaterials; ++i)
		{
			if (!mMaterialVertexBuffers[i].empty())
			{
				createMeshBuffer(mesh, mMaterialVertexBuffers[i], mMaterialIndexBuffers[i], &mMaterials[i]);//, zone);
				mMaterialVertexBuffers[i].clear();
				mMaterialIndexBuffers[i].clear();
			}
			if (!mNoCollisionVertexBuffers[i].empty())
			{
				createMeshBuffer(mesh, mNoCollisionVertexBuffers[i], mNoCollisionIndexBuffers[i], &mMaterials[i]);//, zone);
				mNoCollisionVertexBuffers[i].clear();
				mNoCollisionIndexBuffers[i].clear();
			}
		}

		mesh->recalculateBoundingBox();
		mob->setMesh(n, mesh);
	}

	return mob;
}
