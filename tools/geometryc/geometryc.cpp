/*
 * Copyright 2011-2018 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
 */

#include <stdio.h>

#include <algorithm>
#include <vector>

#include <bx/string.h>
#include <bgfx/bgfx.h>
#include "../../src/vertexdecl.h"

#include <tinystl/allocator.h>
#include <tinystl/unordered_map.h>
#include <tinystl/unordered_set.h>
#include <tinystl/string.h>
namespace stl = tinystl;

#include <forsyth-too/forsythtriangleorderoptimizer.h>
#include <ib-compress/indexbuffercompression.h>

#define BGFX_GEOMETRYC_VERSION_MAJOR 1
#define BGFX_GEOMETRYC_VERSION_MINOR 0

#if 0
#	define BX_TRACE(_format, ...) \
		do { \
			printf(BX_FILE_LINE_LITERAL "BGFX " _format "\n", ##__VA_ARGS__); \
		} while(0)

#	define BX_WARN(_condition, _format, ...) \
		do { \
			if (!(_condition) ) \
			{ \
				BX_TRACE(BX_FILE_LINE_LITERAL "WARN " _format, ##__VA_ARGS__); \
			} \
		} while(0)

#	define BX_CHECK(_condition, _format, ...) \
		do { \
			if (!(_condition) ) \
			{ \
				BX_TRACE(BX_FILE_LINE_LITERAL "CHECK " _format, ##__VA_ARGS__); \
				bx::debugBreak(); \
			} \
		} while(0)
#endif // 0

#include <bx/bx.h>
#include <bx/debug.h>
#include <bx/commandline.h>
#include <bx/timer.h>
#include <bx/hash.h>
#include <bx/uint32_t.h>
#include <bx/math.h>
#include <bx/file.h>

#include "bounds.h"

struct Vector3
{
	float x;
	float y;
	float z;
};

typedef std::vector<Vector3> Vector3Array;

struct Index3
{
	int32_t m_position;
	int32_t m_texcoord;
	int32_t m_normal;
	int32_t m_vertexIndex;
	int32_t m_vbc; // Barycentric ID. Holds eigher 0, 1 or 2.
};

typedef stl::unordered_map<uint64_t, Index3> Index3Map;

struct Triangle
{
	uint64_t m_index[3];
};

typedef std::vector<Triangle> TriangleArray;

struct Group
{
	uint32_t m_startTriangle;
	uint32_t m_numTriangles;
	stl::string m_name;
	stl::string m_material;
};

typedef std::vector<Group> GroupArray;

struct Primitive
{
	uint32_t m_startVertex;
	uint32_t m_startIndex;
	uint32_t m_numVertices;
	uint32_t m_numIndices;
	stl::string m_name;
};

typedef std::vector<Primitive> PrimitiveArray;

static uint32_t s_obbSteps = 17;

#define BGFX_CHUNK_MAGIC_VB  BX_MAKEFOURCC('V', 'B', ' ', 0x1)
#define BGFX_CHUNK_MAGIC_IB  BX_MAKEFOURCC('I', 'B', ' ', 0x0)
#define BGFX_CHUNK_MAGIC_IBC BX_MAKEFOURCC('I', 'B', 'C', 0x0)
#define BGFX_CHUNK_MAGIC_PRI BX_MAKEFOURCC('P', 'R', 'I', 0x0)

long int fsize(FILE* _file)
{
	long int pos = ftell(_file);
	fseek(_file, 0L, SEEK_END);
	long int size = ftell(_file);
	fseek(_file, pos, SEEK_SET);
	return size;
}

void triangleReorder(uint16_t* _indices, uint32_t _numIndices, uint32_t _numVertices, uint16_t _cacheSize)
{
	uint16_t* newIndexList = new uint16_t[_numIndices];
	Forsyth::OptimizeFaces(_indices, _numIndices, _numVertices, 0, newIndexList, _cacheSize);
	bx::memCopy(_indices, newIndexList, _numIndices*2);
	delete [] newIndexList;
}

void triangleCompress(bx::WriterI* _writer, uint16_t* _indices, uint32_t _numIndices, uint8_t* _vertexData, uint32_t _numVertices, uint16_t _stride)
{
	uint32_t* vertexRemap = (uint32_t*)malloc(_numVertices*sizeof(uint32_t) );

	WriteBitstream writer;
	CompressIndexBuffer(_indices, _numIndices/3, vertexRemap, _numVertices, IBCF_AUTO, writer);
	writer.Finish();
	printf( "uncompressed: %10d, compressed: %10d, ratio: %0.2f%%\n"
		, _numIndices*2
		, (uint32_t)writer.ByteSize()
		, 100.0f - float(writer.ByteSize() ) / float(_numIndices*2)*100.0f
		);

	BX_UNUSED(_vertexData, _stride);
	uint8_t* outVertexData = (uint8_t*)malloc(_numVertices*_stride);
	for (uint32_t ii = 0; ii < _numVertices; ++ii)
	{
		uint32_t remap = vertexRemap[ii];
		remap = UINT32_MAX == remap ? ii : remap;
		bx::memCopy(&outVertexData[remap*_stride], &_vertexData[ii*_stride], _stride);
	}
	bx::memCopy(_vertexData, outVertexData, _numVertices*_stride);
	free(outVertexData);

	free(vertexRemap);

	bx::write(_writer, writer.RawData(), (uint32_t)writer.ByteSize() );
}

void calcTangents(void* _vertices, uint16_t _numVertices, bgfx::VertexDecl _decl, const uint16_t* _indices, uint32_t _numIndices)
{
	struct PosTexcoord
	{
		float m_x;
		float m_y;
		float m_z;
		float m_pad0;
		float m_u;
		float m_v;
		float m_pad1;
		float m_pad2;
	};

	float* tangents = new float[6*_numVertices];
	bx::memSet(tangents, 0, 6*_numVertices*sizeof(float) );

	PosTexcoord v0;
	PosTexcoord v1;
	PosTexcoord v2;

	for (uint32_t ii = 0, num = _numIndices/3; ii < num; ++ii)
	{
		const uint16_t* indices = &_indices[ii*3];
		uint32_t i0 = indices[0];
		uint32_t i1 = indices[1];
		uint32_t i2 = indices[2];

		bgfx::vertexUnpack(&v0.m_x, bgfx::Attrib::Position, _decl, _vertices, i0);
		bgfx::vertexUnpack(&v0.m_u, bgfx::Attrib::TexCoord0, _decl, _vertices, i0);

		bgfx::vertexUnpack(&v1.m_x, bgfx::Attrib::Position, _decl, _vertices, i1);
		bgfx::vertexUnpack(&v1.m_u, bgfx::Attrib::TexCoord0, _decl, _vertices, i1);

		bgfx::vertexUnpack(&v2.m_x, bgfx::Attrib::Position, _decl, _vertices, i2);
		bgfx::vertexUnpack(&v2.m_u, bgfx::Attrib::TexCoord0, _decl, _vertices, i2);

		const float bax = v1.m_x - v0.m_x;
		const float bay = v1.m_y - v0.m_y;
		const float baz = v1.m_z - v0.m_z;
		const float bau = v1.m_u - v0.m_u;
		const float bav = v1.m_v - v0.m_v;

		const float cax = v2.m_x - v0.m_x;
		const float cay = v2.m_y - v0.m_y;
		const float caz = v2.m_z - v0.m_z;
		const float cau = v2.m_u - v0.m_u;
		const float cav = v2.m_v - v0.m_v;

		const float det = (bau * cav - bav * cau);
		const float invDet = 1.0f / det;

		const float tx = (bax * cav - cax * bav) * invDet;
		const float ty = (bay * cav - cay * bav) * invDet;
		const float tz = (baz * cav - caz * bav) * invDet;

		const float bx = (cax * bau - bax * cau) * invDet;
		const float by = (cay * bau - bay * cau) * invDet;
		const float bz = (caz * bau - baz * cau) * invDet;

		for (uint32_t jj = 0; jj < 3; ++jj)
		{
			float* tanu = &tangents[indices[jj]*6];
			float* tanv = &tanu[3];
			tanu[0] += tx;
			tanu[1] += ty;
			tanu[2] += tz;

			tanv[0] += bx;
			tanv[1] += by;
			tanv[2] += bz;
		}
	}

	for (uint32_t ii = 0; ii < _numVertices; ++ii)
	{
		const bx::Vec3 tanu = bx::load<bx::Vec3>(&tangents[ii*6]);
		const bx::Vec3 tanv = bx::load<bx::Vec3>(&tangents[ii*6 + 3]);

		float nxyzw[4];
		bgfx::vertexUnpack(nxyzw, bgfx::Attrib::Normal, _decl, _vertices, ii);

		const bx::Vec3 normal  = bx::load<bx::Vec3>(nxyzw);
		const float    ndt     = bx::dot(normal, tanu);
		const bx::Vec3 nxt     = bx::cross(normal, tanu);
		const bx::Vec3 tmp     = bx::sub(tanu, bx::mul(normal, ndt) );

		float tangent[4];
		bx::store(tangent, bx::normalize(tmp) );
		tangent[3] = bx::dot(nxt, tanv) < 0.0f ? -1.0f : 1.0f;

		bgfx::vertexPack(tangent, true, bgfx::Attrib::Tangent, _decl, _vertices, ii);
	}

	delete [] tangents;
}

void write(bx::WriterI* _writer, const void* _vertices, uint32_t _numVertices, uint32_t _stride)
{
	Sphere maxSphere;
	calcMaxBoundingSphere(maxSphere, _vertices, _numVertices, _stride);

	Sphere minSphere;
	calcMinBoundingSphere(minSphere, _vertices, _numVertices, _stride);

	if (minSphere.m_radius > maxSphere.m_radius)
	{
		bx::write(_writer, maxSphere);
	}
	else
	{
		bx::write(_writer, minSphere);
	}

	Aabb aabb;
	toAabb(aabb, _vertices, _numVertices, _stride);
	bx::write(_writer, aabb);

	Obb obb;
	calcObb(obb, _vertices, _numVertices, _stride, s_obbSteps);
	bx::write(_writer, obb);
}

void write(bx::WriterI* _writer
		, const uint8_t* _vertices
		, uint32_t _numVertices
		, const bgfx::VertexDecl& _decl
		, const uint16_t* _indices
		, uint32_t _numIndices
		, const uint8_t* _compressedIndices
		, uint32_t _compressedSize
		, const stl::string& _material
		, const PrimitiveArray& _primitives
		)
{
	using namespace bx;
	using namespace bgfx;

	uint32_t stride = _decl.getStride();
	write(_writer, BGFX_CHUNK_MAGIC_VB);
	write(_writer, _vertices, _numVertices, stride);

	write(_writer, _decl);

	write(_writer, uint16_t(_numVertices) );
	write(_writer, _vertices, _numVertices*stride);

	if (NULL != _compressedIndices)
	{
		write(_writer, BGFX_CHUNK_MAGIC_IBC);
		write(_writer, _numIndices);
		write(_writer, _compressedSize);
		write(_writer, _compressedIndices, _compressedSize);
	}
	else
	{
		write(_writer, BGFX_CHUNK_MAGIC_IB);
		write(_writer, _numIndices);
		write(_writer, _indices, _numIndices*2);
	}

	write(_writer, BGFX_CHUNK_MAGIC_PRI);
	uint16_t nameLen = uint16_t(_material.size() );
	write(_writer, nameLen);
	write(_writer, _material.c_str(), nameLen);
	write(_writer, uint16_t(_primitives.size() ) );
	for (PrimitiveArray::const_iterator primIt = _primitives.begin(); primIt != _primitives.end(); ++primIt)
	{
		const Primitive& prim = *primIt;
		nameLen = uint16_t(prim.m_name.size() );
		write(_writer, nameLen);
		write(_writer, prim.m_name.c_str(), nameLen);
		write(_writer, prim.m_startIndex);
		write(_writer, prim.m_numIndices);
		write(_writer, prim.m_startVertex);
		write(_writer, prim.m_numVertices);
		write(_writer, &_vertices[prim.m_startVertex*stride], prim.m_numVertices, stride);
	}
}

inline uint32_t rgbaToAbgr(uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _a)
{
	return (uint32_t(_r)<<0)
		 | (uint32_t(_g)<<8)
		 | (uint32_t(_b)<<16)
		 | (uint32_t(_a)<<24)
		 ;
}

struct GroupSortByMaterial
{
	bool operator()(const Group& _lhs, const Group& _rhs)
	{
		return 0 < bx::strCmp(_lhs.m_material.c_str(), _rhs.m_material.c_str() );
	}
};

void help(const char* _error = NULL)
{
	if (NULL != _error)
	{
		fprintf(stderr, "Error:\n%s\n\n", _error);
	}

	fprintf(stderr
		, "geometryc, bgfx geometry compiler tool, version %d.%d.%d.\n"
		  "Copyright 2011-2018 Branimir Karadzic. All rights reserved.\n"
		  "License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause\n\n"
		, BGFX_GEOMETRYC_VERSION_MAJOR
		, BGFX_GEOMETRYC_VERSION_MINOR
		, BGFX_API_VERSION
		);

	fprintf(stderr
		, "Usage: geometryc -f <in> -o <out>\n"

		  "\n"
		  "Supported input file types:\n"
		  "    *.obj                  Wavefront\n"

		  "\n"
		  "Options:\n"
		  "  -h, --help               Help.\n"
		  "  -v, --version            Version information only.\n"
		  "  -f <file path>           Input file path.\n"
		  "  -o <file path>           Output file path.\n"
		  "  -s, --scale <num>        Scale factor.\n"
		  "      --ccw                Counter-clockwise winding order.\n"
		  "      --flipv              Flip texture coordinate V.\n"
		  "      --obb <num>          Number of steps for calculating oriented bounding box.\n"
		  "           Default value is 17. Less steps less precise OBB is.\n"
		  "           More steps slower calculation.\n"
		  "      --packnormal <num>   Normal packing.\n"
		  "           0 - unpacked 12 bytes (default).\n"
		  "           1 - packed 4 bytes.\n"
		  "      --packuv <num>       Texture coordinate packing.\n"
		  "           0 - unpacked 8 bytes (default).\n"
		  "           1 - packed 4 bytes.\n"
		  "      --tangent            Calculate tangent vectors (packing mode is the same as normal).\n"
		  "      --barycentric        Adds barycentric vertex attribute (packed in bgfx::Attrib::Color1).\n"
		  "  -c, --compress           Compress indices.\n"

		  "\n"
		  "For additional information, see https://github.com/bkaradzic/bgfx\n"
		);
}

int main(int _argc, const char* _argv[])
{
	bx::CommandLine cmdLine(_argc, _argv);

	if (cmdLine.hasArg('v', "version") )
	{
		fprintf(stderr
			, "geometryc, bgfx geometry compiler tool, version %d.%d.%d.\n"
			, BGFX_GEOMETRYC_VERSION_MAJOR
			, BGFX_GEOMETRYC_VERSION_MINOR
			, BGFX_API_VERSION
			);
		return bx::kExitSuccess;
	}

	if (cmdLine.hasArg('h', "help") )
	{
		help();
		return bx::kExitFailure;
	}

	const char* filePath = cmdLine.findOption('f');
	if (NULL == filePath)
	{
		help("Input file name must be specified.");
		return bx::kExitFailure;
	}

	const char* outFilePath = cmdLine.findOption('o');
	if (NULL == outFilePath)
	{
		help("Output file name must be specified.");
		return bx::kExitFailure;
	}

	float scale = 1.0f;
	const char* scaleArg = cmdLine.findOption('s', "scale");
	if (NULL != scaleArg)
	{
		if (!bx::fromString(&scale, scaleArg))
		{
			scale = 1.0f;
		}
	}

	bool compress = cmdLine.hasArg('c', "compress");

	cmdLine.hasArg(s_obbSteps, '\0', "obb");
	s_obbSteps = bx::uint32_min(bx::uint32_max(s_obbSteps, 1), 90);

	uint32_t packNormal = 0;
	cmdLine.hasArg(packNormal, '\0', "packnormal");

	uint32_t packUv = 0;
	cmdLine.hasArg(packUv, '\0', "packuv");

	bool ccw = cmdLine.hasArg("ccw");
	bool flipV = cmdLine.hasArg("flipv");
	bool hasTangent = cmdLine.hasArg("tangent");
	bool hasBc = cmdLine.hasArg("barycentric");

	FILE* file = fopen(filePath, "r");
	if (NULL == file)
	{
		printf("Unable to open input file '%s'.", filePath);
		exit(bx::kExitFailure);
	}

	int64_t parseElapsed = -bx::getHPCounter();
	int64_t triReorderElapsed = 0;

	uint32_t size = (uint32_t)fsize(file);
	char* data = new char[size+1];
	size = (uint32_t)fread(data, 1, size, file);
	data[size] = '\0';
	fclose(file);

	// https://en.wikipedia.org/wiki/Wavefront_.obj_file

	Vector3Array positions;
	Vector3Array normals;
	Vector3Array texcoords;
	Index3Map indexMap;
	TriangleArray triangles;
	GroupArray groups;

	uint32_t num = 0;

	Group group;
	group.m_startTriangle = 0;
	group.m_numTriangles = 0;

	char commandLine[2048];
	uint32_t len = sizeof(commandLine);
	int argc;
	char* argv[64];

	for (bx::StringView next(data, size); !next.isEmpty(); )
	{
		next = bx::tokenizeCommandLine(next, commandLine, len, argc, argv, BX_COUNTOF(argv), '\n');

		if (0 < argc)
		{
			if (0 == bx::strCmp(argv[0], "#") )
			{
				if (2 < argc
				&&  0 == bx::strCmp(argv[2], "polygons") )
				{
				}
			}
			else if (0 == bx::strCmp(argv[0], "f") )
			{
				Triangle triangle;
				bx::memSet(&triangle, 0, sizeof(Triangle) );

				const int numNormals   = (int)normals.size();
				const int numTexcoords = (int)texcoords.size();
				const int numPositions = (int)positions.size();
				for (uint32_t edge = 0, numEdges = argc-1; edge < numEdges; ++edge)
				{
					Index3 index;
					index.m_texcoord = -1;
					index.m_normal = -1;
					index.m_vertexIndex = -1;
					if (hasBc)
					{
						index.m_vbc = edge < 3 ? edge : (1+(edge+1) )&1;
					}
					else
					{
						index.m_vbc = 0; 
					}

					{
						bx::StringView triplet(argv[edge + 1]);
						bx::StringView vertex(triplet);
						bx::StringView texcoord = bx::strFind(triplet, '/');
						if (!texcoord.isEmpty())
						{
							vertex.set(vertex.getPtr(), texcoord.getPtr());

							const bx::StringView normal = bx::strFind(bx::StringView(texcoord.getPtr() + 1, triplet.getTerm()), '/');
							if (!normal.isEmpty())
							{
								int32_t nn;
								bx::fromString(&nn, bx::StringView(normal.getPtr() + 1, triplet.getTerm()));
								index.m_normal = (nn < 0) ? nn + numNormals : nn - 1;
							}

							texcoord.set(texcoord.getPtr() + 1, normal.getPtr());

							// https://en.wikipedia.org/wiki/Wavefront_.obj_file#Vertex_Normal_Indices_Without_Texture_Coordinate_Indices
							if (!texcoord.isEmpty())
							{
								int32_t tex;
								bx::fromString(&tex, texcoord);
								index.m_texcoord = (tex < 0) ? tex + numTexcoords : tex - 1;
							}
						}

						int32_t pos;
						bx::fromString(&pos, vertex);
						index.m_position = (pos < 0) ? pos + numPositions : pos - 1;
					}

					const uint64_t hash0 = uint64_t(index.m_position)<< 0;
					const uint64_t hash1 = uint64_t(index.m_texcoord)<<20;
					const uint64_t hash2 = uint64_t(index.m_normal  )<<40;
					const uint64_t hash3 = uint64_t(index.m_vbc     )<<60;
					const uint64_t hash  = hash0^hash1^hash2^hash3;

					stl::pair<Index3Map::iterator, bool> result = indexMap.insert(stl::make_pair(hash, index) );
					if (!result.second)
					{
						Index3& oldIndex = result.first->second;
						BX_UNUSED(oldIndex);
						BX_CHECK(true
							&& oldIndex.m_position == index.m_position
							&& oldIndex.m_texcoord == index.m_texcoord
							&& oldIndex.m_normal   == index.m_normal
							, "Hash collision!"
							);
					}

					switch (edge)
					{
					case 0:	case 1:	case 2:
						triangle.m_index[edge] = hash;
						if (2 == edge)
						{
							if (ccw)
							{
								std::swap(triangle.m_index[1], triangle.m_index[2]);
							}
							triangles.push_back(triangle);
						}
						break;

					default:
						if (ccw)
						{
							triangle.m_index[2] = triangle.m_index[1];
							triangle.m_index[1] = hash;
						}
						else
						{
							triangle.m_index[1] = triangle.m_index[2];
							triangle.m_index[2] = hash;
						}

						triangles.push_back(triangle);
						break;
					}
				}
			}
			else if (0 == bx::strCmp(argv[0], "g") )
			{
				group.m_name = argv[1];
			}
			else if (*argv[0] == 'v')
			{
				group.m_numTriangles = (uint32_t)(triangles.size() ) - group.m_startTriangle;
				if (0 < group.m_numTriangles)
				{
					groups.push_back(group);
					group.m_startTriangle = (uint32_t)(triangles.size() );
					group.m_numTriangles = 0;
				}

				if (0 == bx::strCmp(argv[0], "vn") )
				{
					Vector3 normal;
					bx::fromString(&normal.x, argv[1]);
					bx::fromString(&normal.y, argv[2]);
					bx::fromString(&normal.z, argv[3]);

					normals.push_back(normal);
				}
				else if (0 == bx::strCmp(argv[0], "vp") )
				{
					static bool once = true;
					if (once)
					{
						once = false;
						printf("warning: 'parameter space vertices' are unsupported.\n");
					}
				}
				else if (0 == bx::strCmp(argv[0], "vt") )
				{
					Vector3 texcoord;
					texcoord.y = 0.0f;
					texcoord.z = 0.0f;

					bx::fromString(&texcoord.x, argv[1]);

					switch (argc)
					{
					case 4:
						bx::fromString(&texcoord.z, argv[3]);
						BX_FALLTHROUGH;

					case 3:
						bx::fromString(&texcoord.y, argv[2]);
						break;

					default:
						break;
					}

					texcoords.push_back(texcoord);
				}
				else
				{
					float px, py, pz, pw;
					bx::fromString(&px, argv[1]);
					bx::fromString(&py, argv[2]);
					bx::fromString(&pz, argv[3]);

					if (argc > 4)
					{
						bx::fromString(&pw, argv[4]);
					}
					else
					{
						pw = 1.0f;
					}

					float invW = scale/pw;
					px *= invW;
					py *= invW;
					pz *= invW;

					Vector3 pos;
					pos.x = px;
					pos.y = py;
					pos.z = pz;

					positions.push_back(pos);
				}
			}
			else if (0 == bx::strCmp(argv[0], "usemtl") )
			{
				stl::string material(argv[1]);

				if (0 != bx::strCmp(material.c_str(), group.m_material.c_str() ) )
				{
					group.m_numTriangles = (uint32_t)(triangles.size() ) - group.m_startTriangle;
					if (0 < group.m_numTriangles)
					{
						groups.push_back(group);
						group.m_startTriangle = (uint32_t)(triangles.size() );
						group.m_numTriangles = 0;
					}
				}

				group.m_material = material;
			}
// unsupported tags
// 				else if (0 == bx::strCmp(argv[0], "mtllib") )
// 				{
// 				}
// 				else if (0 == bx::strCmp(argv[0], "o") )
// 				{
// 				}
// 				else if (0 == bx::strCmp(argv[0], "s") )
// 				{
// 				}
		}

		++num;
	}

	group.m_numTriangles = (uint32_t)(triangles.size() ) - group.m_startTriangle;
	if (0 < group.m_numTriangles)
	{
		groups.push_back(group);
		group.m_startTriangle = (uint32_t)(triangles.size() );
		group.m_numTriangles = 0;
	}

	delete [] data;

	int64_t now = bx::getHPCounter();
	parseElapsed += now;
	int64_t convertElapsed = -now;

	std::sort(groups.begin(), groups.end(), GroupSortByMaterial() );

	bool hasColor = false;
	bool hasNormal;
	bool hasTexcoord;
	{
		Index3Map::const_iterator it = indexMap.begin();
		hasNormal   = -1 != it->second.m_normal;
		hasTexcoord = -1 != it->second.m_texcoord;

		if (!hasTexcoord)
		{
			for (Index3Map::iterator jt = indexMap.begin(), jtEnd = indexMap.end(); jt != jtEnd && !hasTexcoord; ++jt)
			{
				hasTexcoord |= -1 != jt->second.m_texcoord;
			}

			if (hasTexcoord)
			{
				for (Index3Map::iterator jt = indexMap.begin(), jtEnd = indexMap.end(); jt != jtEnd; ++jt)
				{
					jt->second.m_texcoord = -1 == jt->second.m_texcoord ? 0 : jt->second.m_texcoord;
				}
			}
		}

		if (!hasNormal)
		{
			for (Index3Map::iterator jt = indexMap.begin(), jtEnd = indexMap.end(); jt != jtEnd && !hasNormal; ++jt)
			{
				hasNormal |= -1 != jt->second.m_normal;
			}

			if (hasNormal)
			{
				for (Index3Map::iterator jt = indexMap.begin(), jtEnd = indexMap.end(); jt != jtEnd; ++jt)
				{
					jt->second.m_normal = -1 == jt->second.m_normal ? 0 : jt->second.m_normal;
				}
			}
		}
	}

	bgfx::VertexDecl decl;
	decl.begin();
	decl.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float);

	if (hasColor)
	{
		decl.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true);
	}

	if (hasBc)
	{
		decl.add(bgfx::Attrib::Color1, 4, bgfx::AttribType::Uint8, true);
	}

	if (hasTexcoord)
	{
		switch (packUv)
		{
		default:
		case 0:
			decl.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float);
			break;

		case 1:
			decl.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Half);
			break;
		}
	}

	if (hasNormal)
	{
		hasTangent &= hasTexcoord;

		switch (packNormal)
		{
		default:
		case 0:
			decl.add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float);
			if (hasTangent)
			{
				decl.add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Float);
			}
			break;

		case 1:
			decl.add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Uint8, true, true);
			if (hasTangent)
			{
				decl.add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Uint8, true, true);
			}
			break;
		}
	}

	decl.end();

	uint32_t stride = decl.getStride();
	uint8_t* vertexData = new uint8_t[triangles.size() * 3 * stride];
	uint16_t* indexData = new uint16_t[triangles.size() * 3];
	int32_t numVertices = 0;
	int32_t numIndices = 0;
	int32_t numPrimitives = 0;

	uint8_t* vertices = vertexData;
	uint16_t* indices = indexData;

	stl::string material = groups.begin()->m_material;

	PrimitiveArray primitives;

	bx::FileWriter writer;
	if (!bx::open(&writer, outFilePath) )
	{
		printf("Unable to open output file '%s'.", outFilePath);
		exit(bx::kExitFailure);
	}

	Primitive prim;
	prim.m_startVertex = 0;
	prim.m_startIndex  = 0;

	uint32_t positionOffset = decl.getOffset(bgfx::Attrib::Position);
	uint32_t color0Offset   = decl.getOffset(bgfx::Attrib::Color0);

	bx::DefaultAllocator crtAllocator;
	bx::MemoryBlock  memBlock(&crtAllocator);

	uint32_t ii = 0;
	for (GroupArray::const_iterator groupIt = groups.begin(); groupIt != groups.end(); ++groupIt, ++ii)
	{
		for (uint32_t tri = groupIt->m_startTriangle, end = tri + groupIt->m_numTriangles; tri < end; ++tri)
		{
			if (0 != bx::strCmp(material.c_str(), groupIt->m_material.c_str() )
			||  65533 <= numVertices)
			{
				prim.m_numVertices = numVertices - prim.m_startVertex;
				prim.m_numIndices  = numIndices  - prim.m_startIndex;
				if (0 < prim.m_numVertices)
				{
					primitives.push_back(prim);
				}

				if (hasTangent)
				{
					calcTangents(vertexData, uint16_t(numVertices), decl, indexData, numIndices);
				}

				bx::MemoryWriter memWriter(&memBlock);

				triReorderElapsed -= bx::getHPCounter();
				for (PrimitiveArray::const_iterator primIt = primitives.begin(); primIt != primitives.end(); ++primIt)
				{
					const Primitive& prim1 = *primIt;
					triangleReorder(indexData + prim1.m_startIndex, prim1.m_numIndices, numVertices, 32);
					if (compress)
					{
						triangleCompress(
							  &memWriter
							, indexData  + prim1.m_startIndex
							, prim1.m_numIndices
							, vertexData + prim1.m_startVertex
							, numVertices
							, uint16_t(stride)
							);
					}
				}
				triReorderElapsed += bx::getHPCounter();

				write(&writer
					, vertexData
					, numVertices
					, decl
					, indexData
					, numIndices
					, (uint8_t*)memBlock.more()
					, memBlock.getSize()
					, material
					, primitives
					);
				primitives.clear();

				for (Index3Map::iterator indexIt = indexMap.begin(); indexIt != indexMap.end(); ++indexIt)
				{
					indexIt->second.m_vertexIndex = -1;
				}

				vertices = vertexData;
				indices  = indexData;
				numVertices = 0;
				numIndices  = 0;
				prim.m_startVertex = 0;
				prim.m_startIndex  = 0;
				++numPrimitives;

				material = groupIt->m_material;
			}

			Triangle& triangle = triangles[tri];
			for (uint32_t edge = 0; edge < 3; ++edge)
			{
				uint64_t hash = triangle.m_index[edge];
				Index3& index = indexMap[hash];
				if (index.m_vertexIndex == -1)
				{
		 			index.m_vertexIndex = numVertices++;

					float* position = (float*)(vertices + positionOffset);
					bx::memCopy(position, &positions[index.m_position], 3*sizeof(float) );

					if (hasColor)
					{
						uint32_t* color0 = (uint32_t*)(vertices + color0Offset);
						*color0 = rgbaToAbgr(numVertices%255, numIndices%255, 0, 0xff);
					}

					if (hasBc)
					{
						const float bc[3] =
						{
							(index.m_vbc == 0) ? 1.0f : 0.0f,
							(index.m_vbc == 1) ? 1.0f : 0.0f,
							(index.m_vbc == 2) ? 1.0f : 0.0f,
						};
						bgfx::vertexPack(bc, true, bgfx::Attrib::Color1, decl, vertices);
					}

					if (hasTexcoord)
					{
						float uv[2];
						bx::memCopy(uv, &texcoords[index.m_texcoord], 2*sizeof(float) );

						if (flipV)
						{
							uv[1] = -uv[1];
						}

						bgfx::vertexPack(uv, true, bgfx::Attrib::TexCoord0, decl, vertices);
					}

					if (hasNormal)
					{
						float normal[4];
						bx::store(normal, bx::normalize(bx::load<bx::Vec3>(&normals[index.m_normal]) ) );
						bgfx::vertexPack(normal, true, bgfx::Attrib::Normal, decl, vertices);
					}

					vertices += stride;
				}

				*indices++ = (uint16_t)index.m_vertexIndex;
				++numIndices;
			}
		}

		prim.m_numVertices = numVertices - prim.m_startVertex;
		if (0 < prim.m_numVertices)
		{
			prim.m_numIndices = numIndices - prim.m_startIndex;
			prim.m_name = groupIt->m_name;
			primitives.push_back(prim);
			prim.m_startVertex = numVertices;
			prim.m_startIndex  = numIndices;
		}

		BX_TRACE("%3d: s %5d, n %5d, %s\n"
			, ii
			, groupIt->m_startTriangle
			, groupIt->m_numTriangles
			, groupIt->m_material.c_str()
			);
	}

	if (0 < primitives.size() )
	{
		if (hasTangent)
		{
			calcTangents(vertexData, uint16_t(numVertices), decl, indexData, numIndices);
		}

		bx::MemoryWriter memWriter(&memBlock);

		triReorderElapsed -= bx::getHPCounter();
		for (PrimitiveArray::const_iterator primIt = primitives.begin(); primIt != primitives.end(); ++primIt)
		{
			const Primitive& prim1 = *primIt;
			triangleReorder(indexData + prim1.m_startIndex, prim1.m_numIndices, numVertices, 32);
			if (compress)
			{
				triangleCompress(&memWriter
					, indexData  + prim1.m_startIndex
					, prim1.m_numIndices
					, vertexData + prim1.m_startVertex
					, numVertices
					, uint16_t(stride)
					);
			}
		}
		triReorderElapsed += bx::getHPCounter();

		write(&writer
			, vertexData
			, numVertices
			, decl
			, indexData
			, numIndices
			, (uint8_t*)memBlock.more()
			, memBlock.getSize()
			, material
			, primitives
			);
	}

	printf("size: %d\n", uint32_t(bx::seek(&writer) ) );
	bx::close(&writer);

	delete [] indexData;
	delete [] vertexData;

	now = bx::getHPCounter();
	convertElapsed += now;

	printf("parse %f [s]\ntri reorder %f [s]\nconvert %f [s]\n# %d, g %d, p %d, v %d, i %d\n"
		, double(parseElapsed)/bx::getHPFrequency()
		, double(triReorderElapsed)/bx::getHPFrequency()
		, double(convertElapsed)/bx::getHPFrequency()
		, num
		, uint32_t(groups.size() )
		, numPrimitives
		, numVertices
		, numIndices
		);

	return bx::kExitSuccess;
}
