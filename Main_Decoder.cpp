// test.cpp - bc7enc17.c command line example/test app
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <assert.h>
#include <sstream>
#include <time.h>

#include "bc7enc.h"
#include "lodepng.h"
#include "dds_defs.h"
#include "bc7decomp.h"

#define RGBCX_IMPLEMENTATION
#include "rgbcx.h"

const int MAX_UBER_LEVEL = 5;

inline int iabs(int i) { if (i < 0) i = -i; return i; }
inline uint8_t clamp255(int32_t i) { return (uint8_t)((i & 0xFFFFFF00U) ? (~(i >> 31)) : i); }
template <typename S> inline S clamp(S value, S low, S high) { return (value < low) ? low : ((value > high) ? high : value); }

struct color_quad_u8
{
	uint8_t m_c[4];

	inline color_quad_u8(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		set(r, g, b, a);
	}

	inline color_quad_u8(uint8_t y = 0, uint8_t a = 255)
	{
		set(y, a);
	}

	inline color_quad_u8 &set(uint8_t y, uint8_t a = 255)
	{
		m_c[0] = y;
		m_c[1] = y;
		m_c[2] = y;
		m_c[3] = a;
		return *this;
	}

	inline color_quad_u8 &set(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		m_c[0] = r;
		m_c[1] = g;
		m_c[2] = b;
		m_c[3] = a;
		return *this;
	}

	inline uint8_t &operator[] (uint32_t i) { assert(i < 4);  return m_c[i]; }
	inline uint8_t operator[] (uint32_t i) const { assert(i < 4); return m_c[i]; }

	inline int get_luma() const { return (13938U * m_c[0] + 46869U * m_c[1] + 4729U * m_c[2] + 32768U) >> 16U; } // REC709 weightings
};
typedef std::vector<color_quad_u8> color_quad_u8_vec;

class image_u8
{
public:
	image_u8() :
		m_width(0), m_height(0)
	{
	}

	image_u8(uint32_t width, uint32_t height) :
		m_width(width), m_height(height)
	{
		m_pixels.resize(width * height);
	}

	inline const color_quad_u8_vec &get_pixels() const { return m_pixels; }
	inline color_quad_u8_vec &get_pixels() { return m_pixels; }

	inline uint32_t width() const { return m_width; }
	inline uint32_t height() const { return m_height; }
	inline uint32_t total_pixels() const { return m_width * m_height; }

	inline color_quad_u8 &operator()(uint32_t x, uint32_t y) { assert(x < m_width && y < m_height);  return m_pixels[x + m_width * y]; }
	inline const color_quad_u8 &operator()(uint32_t x, uint32_t y) const { assert(x < m_width && y < m_height);  return m_pixels[x + m_width * y]; }

	image_u8& clear()
	{
		m_width = m_height = 0;
		m_pixels.clear();
		return *this;
	}

	image_u8& init(uint32_t width, uint32_t height)
	{
		clear();

		m_width = width;
		m_height = height;
		m_pixels.resize(width * height);
		return *this;
	}

	image_u8& set_all(const color_quad_u8 &p)
	{
		for (uint32_t i = 0; i < m_pixels.size(); i++)
			m_pixels[i] = p;
		return *this;
	}

	image_u8& crop(uint32_t new_width, uint32_t new_height)
	{
		if ((m_width == new_width) && (m_height == new_height))
			return *this;

		image_u8 new_image(new_width, new_height);

		const uint32_t w = std::min(m_width, new_width);
		const uint32_t h = std::min(m_height, new_height);

		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				new_image(x, y) = (*this)(x, y);

		return swap(new_image);
	}

	image_u8 &swap(image_u8 &other)
	{
		std::swap(m_width, other.m_width);
		std::swap(m_height, other.m_height);
		std::swap(m_pixels, other.m_pixels);
		return *this;
	}

	inline void get_block(uint32_t bx, uint32_t by, uint32_t width, uint32_t height, color_quad_u8 *pPixels)
	{
		assert((bx * width + width) <= m_width);
		assert((by * height + height) <= m_height);

		for (uint32_t y = 0; y < height; y++)
			memcpy(pPixels + y * width, &(*this)(bx * width, by * height + y), width * sizeof(color_quad_u8));
	}

	inline void set_block(uint32_t bx, uint32_t by, uint32_t width, uint32_t height, const color_quad_u8 *pPixels)
	{
		assert((bx * width + width) <= m_width);
		assert((by * height + height) <= m_height);

		for (uint32_t y = 0; y < height; y++)
			memcpy(&(*this)(bx * width, by * height + y), pPixels + y * width, width * sizeof(color_quad_u8));
	}

	image_u8 &swizzle(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
	{
		assert((r | g | b | a) <= 3);
		for (uint32_t y = 0; y < m_height; y++)
		{
			for (uint32_t x = 0; x < m_width; x++)
			{
				color_quad_u8 tmp((*this)(x, y));
				(*this)(x, y).set(tmp[r], tmp[g], tmp[b], tmp[a]);
			}
		}

		return *this;
	}

private:
	color_quad_u8_vec m_pixels;
	uint32_t m_width, m_height;
};

static bool save_png(const char *pFilename, const image_u8 &img, bool save_alpha)
{
	const uint32_t w = img.width();
	const uint32_t h = img.height();

	std::vector<unsigned char> pixels;
	if (save_alpha)
	{
		pixels.resize(w * h * sizeof(color_quad_u8));
		memcpy(&pixels[0], &img.get_pixels()[0], w * h * sizeof(color_quad_u8));
	}
	else
	{
		pixels.resize(w * h * 3);
		unsigned char *pDst = &pixels[0];
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++, pDst += 3)
				pDst[0] = img(x, y)[0], pDst[1] = img(x, y)[1], pDst[2] = img(x, y)[2];
	}

	return lodepng::encode(pFilename, pixels, w, h, save_alpha ? LCT_RGBA : LCT_RGB) == 0;
}

static bool save_dds(const char *pFilename, uint32_t width, uint32_t height, const void *pBlocks, uint32_t pixel_format_bpp, DXGI_FORMAT dxgi_format, bool srgb)
{
	(void)srgb;

	FILE *pFile = NULL;
	pFile = fopen(pFilename, "wb");
	if (!pFile)
	{
		fprintf(stderr, "Failed creating file %s!\n", pFilename);
		return false;
	}

	fwrite("DDS ", 4, 1, pFile);

	DDSURFACEDESC2 desc;
	memset(&desc, 0, sizeof(desc));

	desc.dwSize = sizeof(desc);
	desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CAPS;

	desc.dwWidth = width;
	desc.dwHeight = height;

	desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
	desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);

	desc.ddpfPixelFormat.dwFlags |= DDPF_FOURCC;

	desc.ddpfPixelFormat.dwFourCC = (uint32_t)PIXEL_FMT_FOURCC('D', 'X', '1', '0');
	desc.ddpfPixelFormat.dwRGBBitCount = 0;

	desc.lPitch = (((desc.dwWidth + 3) & ~3) * ((desc.dwHeight + 3) & ~3) * pixel_format_bpp) >> 3;
	desc.dwFlags |= DDSD_LINEARSIZE;

	fwrite(&desc, sizeof(desc), 1, pFile);

	DDS_HEADER_DXT10 hdr10;
	memset(&hdr10, 0, sizeof(hdr10));

	// Not all tools support DXGI_FORMAT_BC7_UNORM_SRGB (like NVTT), but ddsview in DirectXTex pays attention to it. So not sure what to do here.
	// For best compatibility just write DXGI_FORMAT_BC7_UNORM.
	//hdr10.dxgiFormat = srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
	hdr10.dxgiFormat = dxgi_format; // DXGI_FORMAT_BC7_UNORM;
	hdr10.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
	hdr10.arraySize = 1;

	fwrite(&hdr10, sizeof(hdr10), 1, pFile);

	fwrite(pBlocks, desc.lPitch, 1, pFile);

	if (fclose(pFile) == EOF)
	{
		fprintf(stderr, "Failed writing to DDS file %s!\n", pFilename);
		return false;
	}

	return true;
}

//=========================================================================================================================
struct CommandLineArgs
{
	std::string srcFileName;
	std::string destFileName;
	uint32_t width;
	uint32_t height;
	DXGI_FORMAT format;
	uint32_t bytesPerBlock;
	bool sRGB;
};

//parse command line arguments
static void ParseArguments(int argc, char* argv[], CommandLineArgs& outArgs)
{
	int index = 1;//current arg
	while (index + 2 <= argc)
	{
		std::string opt = std::string(argv[index]);
		std::string val = std::string(argv[index + 1]);
		if (opt == "-width")
		{
			outArgs.width = std::atoi(val.c_str());
		}
		else if (opt == "-height")
		{
			outArgs.height = std::atoi(val.c_str());
		}
		else if (opt == "-src")
		{
			outArgs.srcFileName = val;
		}
		else if (opt == "-dest")
		{
			outArgs.destFileName = val;
		}
		else if (opt == "-format")
		{
			// size of block: https://docs.microsoft.com/en-us/windows/win32/direct3d11/texture-block-compression-in-direct3d-11
			if (val == "1")
			{
				outArgs.format = DXGI_FORMAT_BC1_UNORM;
				outArgs.bytesPerBlock = 8;
				std::cout << "FORMAT = DXGI_FORMAT_BC1_UNORM" << std::endl;
			}
			else if (val == "2") 
			{
				outArgs.format = DXGI_FORMAT_BC2_UNORM; 
				outArgs.bytesPerBlock = 16;
				std::cout << "FORMAT = DXGI_FORMAT_BC2_UNORM" << std::endl;
			}
			else if (val == "3")
			{
				outArgs.format = DXGI_FORMAT_BC3_UNORM;
				outArgs.bytesPerBlock = 16;
				std::cout << "FORMAT = DXGI_FORMAT_BC3_UNORM" << std::endl;
			}
			else if (val == "4") 
			{
				outArgs.format = DXGI_FORMAT_BC4_UNORM; 
				outArgs.bytesPerBlock = 8;
				std::cout << "FORMAT = DXGI_FORMAT_BC4_UNORM" << std::endl;
			}
			else if (val == "5") 
			{
				outArgs.format = DXGI_FORMAT_BC5_UNORM; 
				outArgs.bytesPerBlock = 16;
				std::cout << "FORMAT = DXGI_FORMAT_BC5_UNORM" << std::endl;
			}
			else if (val == "7")
			{
				outArgs.format = DXGI_FORMAT_BC7_UNORM;
				outArgs.bytesPerBlock = 16;
				std::cout << "FORMAT = DXGI_FORMAT_BC7_UNORM" << std::endl;
			}
			else
			{
				std::cout << "un-support pixel format." << std::endl;
			}
		}
		else if (opt == "-sRGB")
		{
			outArgs.sRGB = val == "0" ? false : true;
		}

		index += 2;
	}
}

static void ReadRawData(unsigned char* pData, size_t size, std::string fileName)
{
	std::ifstream file(fileName, std::ios::binary);
	if (!file.is_open())
	{
		std::cout << "failed to open file " << fileName << std::endl;
		return;
	}

	file.seekg(0, std::ios::end);
	size_t fileSize = file.tellg();
	file.seekg(0, std::ios::beg);

	std::string strBuffer(fileSize + 1, '\0');
	file.read(&strBuffer[0], fileSize);
	std::replace(strBuffer.begin(), strBuffer.end(), ',', ' ');

	std::stringstream ss;
	ss << strBuffer;
	for (size_t i = 0; i < size; ++i)
	{
		unsigned int c = 0;
		ss >> c;
		pData[i] = c;
	}

}

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		std::cout << "argument count not enough." << std::endl;
		return 0;
	}

	//parse argument
	CommandLineArgs args;
	ParseArguments(argc, argv, args);

	// read Block-Compressed raw data
	size_t byteSize = args.width * args.height * args.bytesPerBlock;
	unsigned char* pRawData = new unsigned char[byteSize];
	ReadRawData(pRawData, byteSize, args.srcFileName);

	rgbcx::bc1_approx_mode bc1_mode = rgbcx::bc1_approx_mode::cBC1Ideal;
	const uint32_t blocks_x = args.width / 4;
	const uint32_t blocks_y = args.height / 4;

	if (args.destFileName.size())
	{
		image_u8 unpacked_image(args.width, args.height);

		bool punchthrough_flag = false;
		for (uint32_t by = 0; by < blocks_y; by++)
		{
			for (uint32_t bx = 0; bx < blocks_x; bx++)
			{
				// raw data to current BC block
				void* pBlock = reinterpret_cast<void*>( &pRawData[args.bytesPerBlock * (bx + by * blocks_x)] );// (args.bytesPerBlock == 16) ? (void *)&packed_image16[bx + by * blocks_x] : (void*)&packed_image8[bx + by * blocks_x];

				color_quad_u8 unpacked_pixels[16];
				for (uint32_t i = 0; i < 16; i++)
					unpacked_pixels[i].set(0, 0, 0, 255);

				switch (args.format)
				{
				case DXGI_FORMAT_BC1_UNORM:
					rgbcx::unpack_bc1(pBlock, unpacked_pixels, true, bc1_mode);
					break;
				case DXGI_FORMAT_BC3_UNORM:
					if (!rgbcx::unpack_bc3(pBlock, unpacked_pixels, bc1_mode))
						punchthrough_flag = true;
					break;
				case DXGI_FORMAT_BC4_UNORM:
					rgbcx::unpack_bc4(pBlock, &unpacked_pixels[0][0], 4);
					break;
				case DXGI_FORMAT_BC5_UNORM:
					rgbcx::unpack_bc5(pBlock, &unpacked_pixels[0][0], 0, 1, 4);
					break;
				case DXGI_FORMAT_BC7_UNORM:
					bc7decomp::unpack_bc7((const uint8_t*)pBlock, (bc7decomp::color_rgba*)unpacked_pixels);
					break;
				default:
					assert(0);
					break;
				}

				// sRGB conversion
				if (args.sRGB)
				{
					for (uint32_t i = 0; i < 16; i++)
					{
						unpacked_pixels[i][0] = 255u * powf(unpacked_pixels[i][0] / 255.0f, 2.2f);
						unpacked_pixels[i][1] = 255u * powf(unpacked_pixels[i][1] / 255.0f, 2.2f);
						unpacked_pixels[i][2] = 255u * powf(unpacked_pixels[i][2] / 255.0f, 2.2f);
						unpacked_pixels[i][3] = 255u * powf(unpacked_pixels[i][3] / 255.0f, 2.2f);
					}

				}

				unpacked_image.set_block(bx, by, 4, 4, unpacked_pixels);
			} // bx
		} // by

		if ((punchthrough_flag) && (args.format == DXGI_FORMAT_BC3_UNORM))
			fprintf(stderr, "Warning: BC3 mode selected, but rgbcx::unpack_bc3() returned one or more blocks using 3-color mode!\n");

		if (bc1_mode != rgbcx::bc1_approx_mode::cBC1Ideal)
			printf("Note: BC1/BC3 RGB decoding was done with the specified vendor's BC1 approximations.\n");

		if (!save_png(args.destFileName.c_str(), unpacked_image, false))
		{
			return EXIT_FAILURE;
		}
		else
		{
			printf("Wrote PNG file %s\n", args.destFileName.c_str());
		}


	}

	return EXIT_SUCCESS;
}
