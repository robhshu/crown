/*
 * Copyright 2014-2015 Daniel Collin. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
 */

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bx/allocator.h>
#include <bx/math.h>
#include <bx/timer.h>
#include <imgui.h>

#include "data/roboto_regular.ttf.h"
#include "data/robotomono_regular.ttf.h"
#include "data/icons_kenney.ttf.h"
#include "data/icons_font_awesome.ttf.h"

#include "imgui_context.h"
#include "device/device.h"
#include "device/pipeline.h"
#include "device/input_types.h"
#include "device/input_device.h"
#include "world/shader_manager.h"
#include "core/strings/string_id.h"

// From bgfx_utils.h
inline bool checkAvailTransientBuffers(uint32_t _numVertices, const bgfx::VertexDecl& _decl, uint32_t _numIndices)
{
	return _numVertices == bgfx::getAvailTransientVertexBuffer(_numVertices, _decl)
		&& _numIndices  == bgfx::getAvailTransientIndexBuffer(_numIndices)
		;
}

struct FontRangeMerge
{
	const void* data;
	size_t      size;
	ImWchar     ranges[3];
};

static FontRangeMerge s_fontRangeMerge[] =
{
	{ s_iconsKenneyTtf,      sizeof(s_iconsKenneyTtf),      { ICON_MIN_KI, ICON_MAX_KI, 0 } },
	{ s_iconsFontAwesomeTtf, sizeof(s_iconsFontAwesomeTtf), { ICON_MIN_FA, ICON_MAX_FA, 0 } },
};

static void* memAlloc(size_t _size);
static void memFree(void* _ptr);

struct ImGuiContext
{
	static void renderDrawLists(ImDrawData* _drawData);

	void render(ImDrawData* _drawData)
	{
		const ImGuiIO& io = ImGui::GetIO();

		const float width  = io.DisplaySize.x;
		const float height = io.DisplaySize.y;

		bgfx::setViewName(m_viewId, "ImGui");
		bgfx::setViewMode(m_viewId, bgfx::ViewMode::Sequential);

		const bgfx::HMD*  hmd  = bgfx::getHMD();
		const bgfx::Caps* caps = bgfx::getCaps();
		if (NULL != hmd && 0 != (hmd->flags & BGFX_HMD_RENDERING) )
		{
			float proj[16];
			bx::mtxProj(proj, hmd->eye[0].fov, 0.1f, 100.0f, bgfx::getCaps()->homogeneousDepth);

			static float time = 0.0f;
			time += 0.05f;

			const float dist = 10.0f;
			const float offset0 = -proj[8] + (hmd->eye[0].viewOffset[0] / dist * proj[0]);
			const float offset1 = -proj[8] + (hmd->eye[1].viewOffset[0] / dist * proj[0]);

			float ortho[2][16];
			const float viewOffset = width/4.0f;
			const float viewWidth  = width/2.0f;
			bx::mtxOrtho(ortho[0], viewOffset, viewOffset + viewWidth, height, 0.0f, 0.0f, 1000.0f, offset0, caps->homogeneousDepth);
			bx::mtxOrtho(ortho[1], viewOffset, viewOffset + viewWidth, height, 0.0f, 0.0f, 1000.0f, offset1, caps->homogeneousDepth);
			bgfx::setViewTransform(m_viewId, NULL, ortho[0], BGFX_VIEW_STEREO, ortho[1]);
			bgfx::setViewRect(m_viewId, 0, 0, hmd->width, hmd->height);
		}
		else
		{
			float ortho[16];
			bx::mtxOrtho(ortho, 0.0f, width, height, 0.0f, 0.0f, 1000.0f, 0.0f, caps->homogeneousDepth);
			bgfx::setViewTransform(m_viewId, NULL, ortho);
			bgfx::setViewRect(m_viewId, 0, 0, uint16_t(width), uint16_t(height) );
		}

		// Render command lists
		for (int32_t ii = 0, num = _drawData->CmdListsCount; ii < num; ++ii)
		{
			bgfx::TransientVertexBuffer tvb;
			bgfx::TransientIndexBuffer tib;

			const ImDrawList* drawList = _drawData->CmdLists[ii];
			uint32_t numVertices = (uint32_t)drawList->VtxBuffer.size();
			uint32_t numIndices  = (uint32_t)drawList->IdxBuffer.size();

			if (!checkAvailTransientBuffers(numVertices, m_decl, numIndices) )
			{
				// not enough space in transient buffer just quit drawing the rest...
				break;
			}

			bgfx::allocTransientVertexBuffer(&tvb, numVertices, m_decl);
			bgfx::allocTransientIndexBuffer(&tib, numIndices);

			ImDrawVert* verts = (ImDrawVert*)tvb.data;
			bx::memCopy(verts, drawList->VtxBuffer.begin(), numVertices * sizeof(ImDrawVert) );

			ImDrawIdx* indices = (ImDrawIdx*)tib.data;
			bx::memCopy(indices, drawList->IdxBuffer.begin(), numIndices * sizeof(ImDrawIdx) );

			uint32_t offset = 0;
			for (const ImDrawCmd* cmd = drawList->CmdBuffer.begin(), *cmdEnd = drawList->CmdBuffer.end(); cmd != cmdEnd; ++cmd)
			{
				if (cmd->UserCallback)
				{
					cmd->UserCallback(drawList, cmd);
				}
				else if (0 != cmd->ElemCount)
				{
					uint64_t state = 0
						| BGFX_STATE_RGB_WRITE
						| BGFX_STATE_ALPHA_WRITE
						| BGFX_STATE_MSAA
						;

					bgfx::TextureHandle th = m_texture;
					crown::StringId32 program = crown::StringId32("ocornut_imgui");

					if (NULL != cmd->TextureId)
					{
						union { ImTextureID ptr; struct { bgfx::TextureHandle handle; uint8_t flags; uint8_t mip; } s; } texture = { cmd->TextureId };
						state |= 0 != (IMGUI_FLAGS_ALPHA_BLEND & texture.s.flags)
							? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)
							: BGFX_STATE_NONE
							;
						th = texture.s.handle;
						if (0 != texture.s.mip)
						{
							const float lodEnabled[4] = { float(texture.s.mip), 1.0f, 0.0f, 0.0f };
							bgfx::setUniform(u_imageLodEnabled, lodEnabled);
							program = crown::StringId32("imgui_image");
						}
					}
					else
					{
						state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);
					}

					const uint16_t xx = uint16_t(bx::max(cmd->ClipRect.x, 0.0f) );
					const uint16_t yy = uint16_t(bx::max(cmd->ClipRect.y, 0.0f) );
					bgfx::setScissor(xx, yy
							, uint16_t(bx::min(cmd->ClipRect.z, 65535.0f)-xx)
							, uint16_t(bx::min(cmd->ClipRect.w, 65535.0f)-yy)
							);

					bgfx::setState(state);
					bgfx::setTexture(0, s_tex, th);
					bgfx::setVertexBuffer(0, &tvb, 0, numVertices);
					bgfx::setIndexBuffer(&tib, offset, cmd->ElemCount);
					crown::device()->_shader_manager->submit(program, VIEW_IMGUI, 0, state);
				}

				offset += cmd->ElemCount;
			}
		}
	}

	void create(float _fontSize, bx::AllocatorI* _allocator)
	{
		m_allocator = _allocator;

		if (NULL == _allocator)
		{
			static bx::DefaultAllocator allocator;
			m_allocator = &allocator;
		}

		m_viewId = VIEW_IMGUI;

		ImGuiIO& io = ImGui::GetIO();
		io.RenderDrawListsFn = renderDrawLists;
		io.MemAllocFn = memAlloc;
		io.MemFreeFn  = memFree;

		io.DisplaySize = ImVec2(1280.0f, 720.0f);
		io.DeltaTime   = 1.0f / 60.0f;
		io.IniFilename = NULL;

		setupStyle(true);

		io.KeyMap[ImGuiKey_Tab]        = (int)crown::KeyboardButton::TAB;
		io.KeyMap[ImGuiKey_LeftArrow]  = (int)crown::KeyboardButton::LEFT;
		io.KeyMap[ImGuiKey_RightArrow] = (int)crown::KeyboardButton::RIGHT;
		io.KeyMap[ImGuiKey_UpArrow]    = (int)crown::KeyboardButton::UP;
		io.KeyMap[ImGuiKey_DownArrow]  = (int)crown::KeyboardButton::DOWN;
		io.KeyMap[ImGuiKey_Home]       = (int)crown::KeyboardButton::HOME;
		io.KeyMap[ImGuiKey_End]        = (int)crown::KeyboardButton::END;
		io.KeyMap[ImGuiKey_Delete]     = (int)crown::KeyboardButton::DEL;
		io.KeyMap[ImGuiKey_Backspace]  = (int)crown::KeyboardButton::BACKSPACE;
		io.KeyMap[ImGuiKey_Enter]      = (int)crown::KeyboardButton::ENTER;
		io.KeyMap[ImGuiKey_Escape]     = (int)crown::KeyboardButton::ESCAPE;
		io.KeyMap[ImGuiKey_A]          = (int)crown::KeyboardButton::A;
		io.KeyMap[ImGuiKey_C]          = (int)crown::KeyboardButton::C;
		io.KeyMap[ImGuiKey_V]          = (int)crown::KeyboardButton::V;
		io.KeyMap[ImGuiKey_X]          = (int)crown::KeyboardButton::X;
		io.KeyMap[ImGuiKey_Y]          = (int)crown::KeyboardButton::Y;
		io.KeyMap[ImGuiKey_Z]          = (int)crown::KeyboardButton::Z;

		bgfx::RendererType::Enum type = bgfx::getRendererType();

		u_imageLodEnabled = bgfx::createUniform("u_imageLodEnabled", bgfx::UniformType::Vec4);

		m_decl
			.begin()
			.add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
			.end();

		s_tex = bgfx::createUniform("s_tex", bgfx::UniformType::Int1);

		uint8_t* data;
		int32_t width;
		int32_t height;
		{
			ImFontConfig config;
			config.FontDataOwnedByAtlas = false;
			config.MergeMode = false;
//			config.MergeGlyphCenterV = true;

			const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();
			m_font[ImGui::Font::Regular] = io.Fonts->AddFontFromMemoryTTF( (void*)s_robotoRegularTtf,     sizeof(s_robotoRegularTtf),     _fontSize,      &config, ranges);
			m_font[ImGui::Font::Mono   ] = io.Fonts->AddFontFromMemoryTTF( (void*)s_robotoMonoRegularTtf, sizeof(s_robotoMonoRegularTtf), _fontSize-3.0f, &config, ranges);

			config.MergeMode = true;
			config.DstFont   = m_font[ImGui::Font::Regular];

			for (uint32_t ii = 0; ii < BX_COUNTOF(s_fontRangeMerge); ++ii)
			{
				const FontRangeMerge& frm = s_fontRangeMerge[ii];

				io.Fonts->AddFontFromMemoryTTF( (void*)frm.data
						, (int)frm.size
						, _fontSize-3.0f
						, &config
						, frm.ranges
						);
			}
		}

		io.Fonts->GetTexDataAsRGBA32(&data, &width, &height);

		m_texture = bgfx::createTexture2D(
			  (uint16_t)width
			, (uint16_t)height
			, false
			, 1
			, bgfx::TextureFormat::BGRA8
			, 0
			, bgfx::copy(data, width*height*4)
			);

		ImGui::InitDockContext();
	}

	void destroy()
	{
		ImGui::ShutdownDockContext();
		ImGui::Shutdown();

		bgfx::destroy(s_tex);
		bgfx::destroy(m_texture);

		bgfx::destroy(u_imageLodEnabled);

		m_allocator = NULL;
	}

	void setupStyle(bool _dark)
	{
		// Doug Binks' darl color scheme
		// https://gist.github.com/dougbinks/8089b4bbaccaaf6fa204236978d165a9
		ImGuiStyle& style = ImGui::GetStyle();
		if (_dark)
		{
			ImGui::StyleColorsDark(&style);
		}
		else
		{
			ImGui::StyleColorsLight(&style);
		}

		style.FrameRounding = 4.0f;
	}

	void beginFrame(uint8_t view_id, uint16_t width, uint16_t height)
	{
		m_viewId = view_id;

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(width, height);
		io.DeltaTime   = 1.0f / 60.0f;

		ImGui::NewFrame();
	}

	void endFrame()
	{
		ImGui::Render();
	}

	bx::AllocatorI*     m_allocator;
	bgfx::VertexDecl    m_decl;
	bgfx::TextureHandle m_texture;
	bgfx::UniformHandle s_tex;
	bgfx::UniformHandle u_imageLodEnabled;
	ImFont* m_font[ImGui::Font::Count];
	bgfx::ViewId m_viewId;
};

static ImGuiContext s_ctx;

static void* memAlloc(size_t _size)
{
	return BX_ALLOC(s_ctx.m_allocator, _size);
}

static void memFree(void* _ptr)
{
	BX_FREE(s_ctx.m_allocator, _ptr);
}

void ImGuiContext::renderDrawLists(ImDrawData* _drawData)
{
	s_ctx.render(_drawData);
}

namespace ImGui
{
	void PushFont(Font::Enum _font)
	{
		PushFont(s_ctx.m_font[_font]);
	}
} // namespace ImGui


namespace crown
{

void imgui_create(f32 _fontSize, bx::AllocatorI* _allocator)
{
	s_ctx.create(_fontSize, _allocator);
}

void imgui_destroy()
{
	s_ctx.destroy();
}

void imgui_begin_frame(uint8_t view_id, u16 width, u16 height)
{
	s_ctx.beginFrame(view_id, width, height);
}

void imgui_end_frame()
{
	s_ctx.endFrame();
}

} // namespace crown

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4505); // error C4505: '' : unreferenced local function has been removed
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wunused-function"); // warning: ‘int rect_width_compare(const void*, const void*)’ defined but not used
BX_PRAGMA_DIAGNOSTIC_PUSH();
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG("-Wunknown-pragmas")
//BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wunused-but-set-variable"); // warning: variable ‘L1’ set but not used
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wtype-limits"); // warning: comparison is always true due to limited range of data type
#define STBTT_malloc(_size, _userData) memAlloc(_size)
#define STBTT_free(_ptr, _userData) memFree(_ptr)
#define STB_RECT_PACK_IMPLEMENTATION
#include <stb/stb_rect_pack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>
BX_PRAGMA_DIAGNOSTIC_POP();
