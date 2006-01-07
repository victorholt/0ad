
///////////////////////////////////////////////////////////////////////////////
//
// Name:		Renderer.cpp
// Author:		Rich Cross
// Contact:		rich@wildfiregames.com
//
// Description: OpenGL renderer class; a higher level interface
//	on top of OpenGL to handle rendering the basic visual games
//	types - terrain, models, sprites, particles etc
//
///////////////////////////////////////////////////////////////////////////////


#include "precompiled.h"

#include <map>
#include <set>
#include <algorithm>
#include "Renderer.h"
#include "Terrain.h"
#include "Matrix3D.h"
#include "MathUtil.h"
#include "Camera.h"
#include "Texture.h"
#include "LightEnv.h"
#include "Terrain.h"
#include "CLogger.h"
#include "ps/Game.h"
#include "Profile.h"
#include "Game.h"
#include "World.h"
#include "Player.h"
#include "LOSManager.h"

#include "Model.h"
#include "ModelDef.h"

#include "ogl.h"
#include "lib/res/res.h"
#include "lib/res/file/file.h"
#include "lib/res/graphics/tex.h"
#include "lib/res/graphics/ogl_tex.h"
#include "ps/Loader.h"
#include "ps/ProfileViewer.h"

#include "renderer/FixedFunctionModelRenderer.h"
#include "renderer/HWLightingModelRenderer.h"
#include "renderer/InstancingModelRenderer.h"
#include "renderer/ModelRenderer.h"
#include "renderer/PlayerRenderer.h"
#include "renderer/RenderModifiers.h"
#include "renderer/RenderPathVertexShader.h"
#include "renderer/ShadowMap.h"
#include "renderer/TerrainRenderer.h"
#include "renderer/TransparencyRenderer.h"
#include "renderer/WaterManager.h"

#define LOG_CATEGORY "graphics"


///////////////////////////////////////////////////////////////////////////////////
// CRendererStatsTable - Profile display of rendering stats

/**
 * Class CRendererStatsTable: Implementation of AbstractProfileTable to
 * display the renderer stats in-game.
 *
 * Accesses CRenderer::m_Stats by keeping the reference passed to the
 * constructor.
 */
class CRendererStatsTable : public AbstractProfileTable
{
public:
	CRendererStatsTable(const CRenderer::Stats& st);

	// Implementation of AbstractProfileTable interface
	CStr GetName();
	CStr GetTitle();
	uint GetNumberRows();
	const std::vector<ProfileColumn>& GetColumns();
	CStr GetCellText(uint row, uint col);
	AbstractProfileTable* GetChild(uint row);

private:
	/// Reference to the renderer singleton's stats
	const CRenderer::Stats& Stats;

	/// Column descriptions
	std::vector<ProfileColumn> columnDescriptions;

	enum {
		Row_Counter = 0,
		Row_DrawCalls,
		Row_TerrainTris,
		Row_ModelTris,
		Row_BlendSplats,

		// Must be last to count number of rows
		NumberRows
	};

	// no copy ctor because some members are const
	CRendererStatsTable& operator=(const CRendererStatsTable&);
};

// Construction
CRendererStatsTable::CRendererStatsTable(const CRenderer::Stats& st)
	: Stats(st)
{
	columnDescriptions.push_back(ProfileColumn("Name", 230));
	columnDescriptions.push_back(ProfileColumn("Value", 100));
}

// Implementation of AbstractProfileTable interface
CStr CRendererStatsTable::GetName()
{
	return "renderer";
}

CStr CRendererStatsTable::GetTitle()
{
	return "Renderer statistics";
}

uint CRendererStatsTable::GetNumberRows()
{
	return NumberRows;
}

const std::vector<ProfileColumn>& CRendererStatsTable::GetColumns()
{
	return columnDescriptions;
}

CStr CRendererStatsTable::GetCellText(uint row, uint col)
{
	char buf[256];

	switch(row)
	{
	case Row_Counter:
		if (col == 0)
			return "counter";
		snprintf(buf, sizeof(buf), "%d", Stats.m_Counter);
		return buf;

	case Row_DrawCalls:
		if (col == 0)
			return "# draw calls";
		snprintf(buf, sizeof(buf), "%d", Stats.m_DrawCalls);
		return buf;

	case Row_TerrainTris:
		if (col == 0)
			return "# terrain tris";
		snprintf(buf, sizeof(buf), "%d", Stats.m_TerrainTris);
		return buf;

	case Row_ModelTris:
		if (col == 0)
			return "# model tris";
		snprintf(buf, sizeof(buf), "%d", Stats.m_ModelTris);
		return buf;

	case Row_BlendSplats:
		if (col == 0)
			return "# blend splats";
		snprintf(buf, sizeof(buf), "%d", Stats.m_BlendSplats);
		return buf;

	default:
		return "???";
	}
}

AbstractProfileTable* CRendererStatsTable::GetChild(uint UNUSED(row))
{
	return 0;
}


///////////////////////////////////////////////////////////////////////////////////
// CRenderer implementation

/**
 * Struct CRendererInternals: Truly hide data that is supposed to be hidden
 * in this structure so it won't even appear in header files.
 */
struct CRendererInternals
{
	/// Table to display renderer stats in-game via profile system
	CRendererStatsTable profileTable;

	/// Water manager
	WaterManager waterManager;

	/// Terrain renderer
	TerrainRenderer* terrainRenderer;

	/// Shadow map
	ShadowMap* shadow;
	
	CRendererInternals()
	: profileTable(g_Renderer.m_Stats)
	{
		terrainRenderer = new TerrainRenderer();
		shadow = new ShadowMap();
	}

	~CRendererInternals()
	{
		delete shadow;
		delete terrainRenderer;
	}
};

///////////////////////////////////////////////////////////////////////////////////
// CRenderer destructor
CRenderer::CRenderer()
{
	m = new CRendererInternals;
	m_WaterManager = &m->waterManager;

	g_ProfileViewer.AddRootTable(&m->profileTable);

	m_Width=0;
	m_Height=0;
	m_Depth=0;
	m_FrameCounter=0;
	m_TerrainRenderMode=SOLID;
	m_ModelRenderMode=SOLID;
	m_ClearColor[0]=m_ClearColor[1]=m_ClearColor[2]=m_ClearColor[3]=0;

	m_SortAllTransparent = false;
	m_FastNormals = true;

	m_VertexShader = 0;

	m_Options.m_NoVBO=false;
	m_Options.m_Shadows=true;
	m_Options.m_ShadowColor=RGBAColor(0.4f,0.4f,0.4f,1.0f);
	m_Options.m_RenderPath = RP_DEFAULT;

	for (uint i=0;i<MaxTextureUnits;i++) {
		m_ActiveTextures[i]=0;
	}

	// Must query card capabilities before creating renderers that depend
	// on card capabilities.
	EnumCaps();

	m_VertexShader = new RenderPathVertexShader;
	if (!m_VertexShader->Init())
	{
		delete m_VertexShader;
		m_VertexShader = 0;
	}

	// model rendering
	m_Models.VertexFF = ModelVertexRendererPtr(new FixedFunctionModelRenderer);
	if (HWLightingModelRenderer::IsAvailable())
		m_Models.VertexHWLit = ModelVertexRendererPtr(new HWLightingModelRenderer);
	if (InstancingModelRenderer::IsAvailable())
		m_Models.VertexInstancing = ModelVertexRendererPtr(new InstancingModelRenderer);
	m_Models.VertexPolygonSort = ModelVertexRendererPtr(new PolygonSortModelRenderer);

	m_Models.NormalFF = new BatchModelRenderer(m_Models.VertexFF);
	m_Models.PlayerFF = new BatchModelRenderer(m_Models.VertexFF);
	m_Models.TranspFF = new SortModelRenderer(m_Models.VertexFF);
	if (m_Models.VertexHWLit)
	{
		m_Models.NormalHWLit = new BatchModelRenderer(m_Models.VertexHWLit);
		m_Models.PlayerHWLit = new BatchModelRenderer(m_Models.VertexHWLit);
		m_Models.TranspHWLit = new SortModelRenderer(m_Models.VertexHWLit);
	}
	else
	{
		m_Models.NormalHWLit = NULL;
		m_Models.PlayerHWLit = NULL;
		m_Models.TranspHWLit = NULL;
	}
	if (m_Models.VertexInstancing)
	{
		m_Models.NormalInstancing = new BatchModelRenderer(m_Models.VertexInstancing);
		m_Models.PlayerInstancing = new BatchModelRenderer(m_Models.VertexInstancing);
	}
	else
	{
		m_Models.NormalInstancing = NULL;
		m_Models.PlayerInstancing = NULL;
	}
	m_Models.TranspSortAll = new SortModelRenderer(m_Models.VertexPolygonSort);

	m_Models.ModWireframe = RenderModifierPtr(new WireframeRenderModifier);
	m_Models.ModPlain = RenderModifierPtr(new PlainRenderModifier);
	SetFastPlayerColor(true);
	m_Models.ModSolidColor = RenderModifierPtr(new SolidColorRenderModifier);
	m_Models.ModTransparent = RenderModifierPtr(new TransparentRenderModifier);
	m_Models.ModTransparentShadow = RenderModifierPtr(new TransparentShadowRenderModifier);

	ONCE( ScriptingInit(); );
}

///////////////////////////////////////////////////////////////////////////////////
// CRenderer destructor
CRenderer::~CRenderer()
{
	// model rendering
	delete m_Models.NormalFF;
	delete m_Models.PlayerFF;
	delete m_Models.TranspFF;
	delete m_Models.NormalHWLit;
	delete m_Models.PlayerHWLit;
	delete m_Models.TranspHWLit;
	delete m_Models.NormalInstancing;
	delete m_Models.PlayerInstancing;
	delete m_Models.TranspSortAll;

	// general
	delete m_VertexShader;
	m_VertexShader = 0;

	// we no longer UnloadAlphaMaps / UnloadWaterTextures here -
	// that is the responsibility of the module that asked for
	// them to be loaded (i.e. CGameView).
	delete m;
}


///////////////////////////////////////////////////////////////////////////////////
// EnumCaps: build card cap bits
void CRenderer::EnumCaps()
{
	// assume support for nothing
	m_Caps.m_VBO=false;
	m_Caps.m_TextureBorderClamp=false;
	m_Caps.m_GenerateMipmaps=false;
	m_Caps.m_VertexShader=false;

	// now start querying extensions
	if (!m_Options.m_NoVBO) {
		if (oglHaveExtension("GL_ARB_vertex_buffer_object")) {
			m_Caps.m_VBO=true;
		}
	}
	if (oglHaveExtension("GL_ARB_texture_border_clamp")) {
		m_Caps.m_TextureBorderClamp=true;
	}
	if (oglHaveExtension("GL_SGIS_generate_mipmap")) {
		m_Caps.m_GenerateMipmaps=true;
	}
	if (0 == oglHaveExtensions(0, "GL_ARB_shader_objects", "GL_ARB_shading_language_100", 0))
	{
		if (oglHaveExtension("GL_ARB_vertex_shader"))
			m_Caps.m_VertexShader=true;
	}
}


bool CRenderer::Open(int width, int height, int depth)
{
	m_Width = width;
	m_Height = height;
	m_Depth = depth;

	// set packing parameters
	glPixelStorei(GL_PACK_ALIGNMENT,1);
	glPixelStorei(GL_UNPACK_ALIGNMENT,1);

	// setup default state
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_DEPTH_TEST);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glEnable(GL_CULL_FACE);

	GLint bits;
	glGetIntegerv(GL_DEPTH_BITS,&bits);
	LOG(NORMAL, LOG_CATEGORY, "CRenderer::Open: depth bits %d",bits);
	glGetIntegerv(GL_STENCIL_BITS,&bits);
	LOG(NORMAL, LOG_CATEGORY, "CRenderer::Open: stencil bits %d",bits);
	glGetIntegerv(GL_ALPHA_BITS,&bits);
	LOG(NORMAL, LOG_CATEGORY, "CRenderer::Open: alpha bits %d",bits);

	if (m_Options.m_RenderPath == RP_DEFAULT)
		SetRenderPath(m_Options.m_RenderPath);

	return true;
}

// resize renderer view
void CRenderer::Resize(int width,int height)
{
	// need to recreate the shadow map object to resize the shadow texture
	delete m->shadow;
	m->shadow = new ShadowMap;

	m_Width = width;
	m_Height = height;
}

//////////////////////////////////////////////////////////////////////////////////////////
// SetOptionBool: set boolean renderer option
void CRenderer::SetOptionBool(enum Option opt,bool value)
{
	switch (opt) {
		case OPT_NOVBO:
			m_Options.m_NoVBO=value;
			break;
		case OPT_SHADOWS:
			m_Options.m_Shadows=value;
			break;
		case OPT_NOPBUFFER:
			// NOT IMPLEMENTED
			break;
		default:
			debug_warn("CRenderer::SetOptionBool: unknown option");
			break;
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
// GetOptionBool: get boolean renderer option
bool CRenderer::GetOptionBool(enum Option opt) const
{
	switch (opt) {
		case OPT_NOVBO:
			return m_Options.m_NoVBO;
		case OPT_SHADOWS:
			return m_Options.m_Shadows;
		default:
			debug_warn("CRenderer::GetOptionBool: unknown option");
			break;
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////////////////////
// SetOptionColor: set color renderer option
void CRenderer::SetOptionColor(enum Option opt,const RGBAColor& value)
{
	switch (opt) {
		case OPT_SHADOWCOLOR:
			m_Options.m_ShadowColor=value;
			break;
		default:
			debug_warn("CRenderer::SetOptionColor: unknown option");
			break;
	}
}

void CRenderer::SetOptionFloat(enum Option opt, float val)
{
	switch(opt)
	{
	case OPT_LODBIAS:
		m_Options.m_LodBias = val;
		break;
	default:
		debug_warn("CRenderer::SetOptionFloat: unknown option");
		break;
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
// GetOptionColor: get color renderer option
const RGBAColor& CRenderer::GetOptionColor(enum Option opt) const
{
	static const RGBAColor defaultColor(1.0f,1.0f,1.0f,1.0f);

	switch (opt) {
		case OPT_SHADOWCOLOR:
			return m_Options.m_ShadowColor;
		default:
			debug_warn("CRenderer::GetOptionColor: unknown option");
			break;
	}

	return defaultColor;
}


//////////////////////////////////////////////////////////////////////////////////////////
// SetRenderPath: Select the preferred render path.
// This may only be called before Open(), because the layout of vertex arrays and other
// data may depend on the chosen render path.
void CRenderer::SetRenderPath(RenderPath rp)
{
	if (rp == RP_DEFAULT)
	{
		if (m_Models.NormalHWLit && m_Models.PlayerHWLit)
			rp = RP_VERTEXSHADER;
		else
			rp = RP_FIXED;
	}

	if (rp == RP_VERTEXSHADER)
	{
		if (!m_Models.NormalHWLit || !m_Models.PlayerHWLit)
		{
			LOG(WARNING, LOG_CATEGORY, "Falling back to fixed function\n");
			rp = RP_FIXED;
		}
	}

	m_Options.m_RenderPath = rp;
}


CStr CRenderer::GetRenderPathName(RenderPath rp)
{
	switch(rp) {
	case RP_DEFAULT: return "default";
	case RP_FIXED: return "fixed";
	case RP_VERTEXSHADER: return "vertexshader";
	default: return "(invalid)";
	}
}

CRenderer::RenderPath CRenderer::GetRenderPathByName(CStr name)
{
	if (name == "fixed")
		return RP_FIXED;
	if (name == "vertexshader")
		return RP_VERTEXSHADER;
	if (name == "default")
		return RP_DEFAULT;

	LOG(WARNING, LOG_CATEGORY, "Unknown render path name '%hs', assuming 'default'", name.c_str());
	return RP_DEFAULT;
}


//////////////////////////////////////////////////////////////////////////////////////////
// SetFastPlayerColor
void CRenderer::SetFastPlayerColor(bool fast)
{
	m_FastPlayerColor = fast;

	if (m_FastPlayerColor)
	{
		if (!FastPlayerColorRender::IsAvailable())
		{
			LOG(WARNING, LOG_CATEGORY, "Falling back to slower player color rendering.");
			m_FastPlayerColor = false;
		}
	}

	if (m_FastPlayerColor)
		m_Models.ModPlayer = RenderModifierPtr(new FastPlayerColorRender);
	else
		m_Models.ModPlayer = RenderModifierPtr(new SlowPlayerColorRender);
}

//////////////////////////////////////////////////////////////////////////////////////////
// BeginFrame: signal frame start
void CRenderer::BeginFrame()
{
#ifndef SCED
	if(!g_Game || !g_Game->IsGameStarted())
		return;
#endif

	// bump frame counter
	m_FrameCounter++;

	if (m_VertexShader)
		m_VertexShader->BeginFrame();

	// zero out all the per-frame stats
	m_Stats.Reset();

	// calculate coefficients for terrain and unit lighting
	m_SHCoeffsUnits.Clear();
	m_SHCoeffsTerrain.Clear();

	if (m_LightEnv) {
		m_SHCoeffsUnits.AddDirectionalLight(m_LightEnv->m_SunDir, m_LightEnv->m_SunColor);
		m_SHCoeffsTerrain.AddDirectionalLight(m_LightEnv->m_SunDir, m_LightEnv->m_SunColor);

		m_SHCoeffsUnits.AddAmbientLight(m_LightEnv->m_UnitsAmbientColor);
		m_SHCoeffsTerrain.AddAmbientLight(m_LightEnv->m_TerrainAmbientColor);
	}

	// init per frame stuff
	m_ShadowRendered=false;
	m_ShadowBound.SetEmpty();
}

//////////////////////////////////////////////////////////////////////////////////////////
// SetClearColor: set color used to clear screen in BeginFrame()
void CRenderer::SetClearColor(u32 color)
{
	m_ClearColor[0]=float(color & 0xff)/255.0f;
	m_ClearColor[1]=float((color>>8) & 0xff)/255.0f;
	m_ClearColor[2]=float((color>>16) & 0xff)/255.0f;
	m_ClearColor[3]=float((color>>24) & 0xff)/255.0f;
}

void CRenderer::RenderShadowMap()
{
	PROFILE( "render shadow map" );

	m->shadow->SetupFrame(m_ShadowBound);
	m->shadow->BeginRender();
	
	// TODO HACK fold this into ShadowMap
	glColor4fv(m_Options.m_ShadowColor);

	glDisable(GL_CULL_FACE);

	m_Models.NormalFF->Render(m_Models.ModSolidColor, MODELFLAG_CASTSHADOWS);
	m_Models.PlayerFF->Render(m_Models.ModSolidColor, MODELFLAG_CASTSHADOWS);
	if (m_Models.NormalHWLit)
		m_Models.NormalHWLit->Render(m_Models.ModSolidColor, MODELFLAG_CASTSHADOWS);
	if (m_Models.PlayerHWLit)
		m_Models.PlayerHWLit->Render(m_Models.ModSolidColor, MODELFLAG_CASTSHADOWS);
	if (m_Models.NormalInstancing)
		m_Models.NormalInstancing->Render(m_Models.ModSolidColor, MODELFLAG_CASTSHADOWS);
	if (m_Models.PlayerInstancing)
		m_Models.PlayerInstancing->Render(m_Models.ModSolidColor, MODELFLAG_CASTSHADOWS);
	m_Models.TranspFF->Render(m_Models.ModTransparentShadow, MODELFLAG_CASTSHADOWS);
	if (m_Models.TranspHWLit)
		m_Models.TranspHWLit->Render(m_Models.ModTransparentShadow, MODELFLAG_CASTSHADOWS);
	m_Models.TranspSortAll->Render(m_Models.ModTransparentShadow, MODELFLAG_CASTSHADOWS);

	glEnable(GL_CULL_FACE);

	glColor3f(1.0f,1.0f,1.0f);

	m->shadow->EndRender();
}

//TODO: Fold into TerrainRenderer
void CRenderer::ApplyShadowMap()
{
	PROFILE( "applying shadows" );

	const CMatrix3D& texturematrix = m->shadow->GetTextureMatrix();
	GLuint shadowmap = m->shadow->GetTexture();

	glMatrixMode(GL_TEXTURE);
	glLoadMatrixf(&texturematrix._11);
	glMatrixMode(GL_MODELVIEW);

	m->terrainRenderer->ApplyShadowMap(shadowmap);

	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
}

void CRenderer::RenderPatches()
{
	PROFILE(" render patches ");

	// switch on wireframe if we need it
	if (m_TerrainRenderMode==WIREFRAME) {
		MICROLOG(L"wireframe on");
		glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
	}

	// render all the patches, including blend pass
	MICROLOG(L"render patch submissions");
	m->terrainRenderer->RenderTerrain();

	if (m_TerrainRenderMode==WIREFRAME) {
		// switch wireframe off again
		MICROLOG(L"wireframe off");
		glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
	} else if (m_TerrainRenderMode==EDGED_FACES) {
		// edged faces: need to make a second pass over the data:
		// first switch on wireframe
		glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);

		// setup some renderstate ..
		glDepthMask(0);
		SetTexture(0,0);
		glColor4f(1,1,1,0.35f);
		glLineWidth(2.0f);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

		// render tiles edges
		m->terrainRenderer->RenderPatches();

		// set color for outline
		glColor3f(0,0,1);
		glLineWidth(4.0f);

		// render outline of each patch
		m->terrainRenderer->RenderOutlines();

		// .. and restore the renderstates
		glDisable(GL_BLEND);
		glDepthMask(1);

		// restore fill mode, and we're done
		glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
	}
}

void CRenderer::RenderModels()
{
	PROFILE( "render models ");

	// switch on wireframe if we need it
	if (m_ModelRenderMode==WIREFRAME) {
		glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
	}

	m_Models.NormalFF->Render(m_Models.ModPlain, 0);
	m_Models.PlayerFF->Render(m_Models.ModPlayer, 0);
	if (m_Models.NormalHWLit)
		m_Models.NormalHWLit->Render(m_Models.ModPlain, 0);
	if (m_Models.PlayerHWLit)
		m_Models.PlayerHWLit->Render(m_Models.ModPlayer, 0);
	if (m_Models.NormalInstancing)
		m_Models.NormalInstancing->Render(m_Models.ModPlain, 0);
	if (m_Models.PlayerInstancing)
		m_Models.PlayerInstancing->Render(m_Models.ModPlayer, 0);

	if (m_ModelRenderMode==WIREFRAME) {
		// switch wireframe off again
		glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
	} else if (m_ModelRenderMode==EDGED_FACES) {
		m_Models.NormalFF->Render(m_Models.ModWireframe, 0);
		m_Models.PlayerFF->Render(m_Models.ModWireframe, 0);
		if (m_Models.NormalHWLit)
			m_Models.NormalHWLit->Render(m_Models.ModWireframe, 0);
		if (m_Models.PlayerHWLit)
			m_Models.PlayerHWLit->Render(m_Models.ModWireframe, 0);
		if (m_Models.NormalInstancing)
			m_Models.NormalInstancing->Render(m_Models.ModWireframe, 0);
		if (m_Models.PlayerInstancing)
			m_Models.PlayerInstancing->Render(m_Models.ModWireframe, 0);
	}
}

void CRenderer::RenderTransparentModels()
{
	PROFILE( "render transparent models ");

	// switch on wireframe if we need it
	if (m_ModelRenderMode==WIREFRAME) {
		glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
	}

	m_Models.TranspFF->Render(m_Models.ModTransparent, 0);
	if (m_Models.TranspHWLit)
		m_Models.TranspHWLit->Render(m_Models.ModTransparent, 0);
	m_Models.TranspSortAll->Render(m_Models.ModTransparent, 0);

	if (m_ModelRenderMode==WIREFRAME) {
		// switch wireframe off again
		glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
	} else if (m_ModelRenderMode==EDGED_FACES) {
		m_Models.TranspFF->Render(m_Models.ModWireframe, 0);
		if (m_Models.TranspHWLit)
			m_Models.TranspHWLit->Render(m_Models.ModWireframe, 0);
		m_Models.TranspSortAll->Render(m_Models.ModWireframe, 0);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FlushFrame: force rendering of any batched objects
void CRenderer::FlushFrame()
{
#ifndef SCED
	if(!g_Game || !g_Game->IsGameStarted())
		return;
#endif

	oglCheck();

	// Prepare model renderers
	PROFILE_START("prepare models");
	m_Models.NormalFF->PrepareModels();
	m_Models.PlayerFF->PrepareModels();
	m_Models.TranspFF->PrepareModels();
	if (m_Models.NormalHWLit)
		m_Models.NormalHWLit->PrepareModels();
	if (m_Models.PlayerHWLit)
		m_Models.PlayerHWLit->PrepareModels();
	if (m_Models.TranspHWLit)
		m_Models.TranspHWLit->PrepareModels();
	if (m_Models.NormalInstancing)
		m_Models.NormalInstancing->PrepareModels();
	if (m_Models.PlayerInstancing)
		m_Models.PlayerInstancing->PrepareModels();
	m_Models.TranspSortAll->PrepareModels();
	PROFILE_END("prepare models");

	PROFILE_START("prepare terrain");
	m->terrainRenderer->PrepareForRendering();
	PROFILE_END("prepare terrain");

	if (!m_ShadowRendered) {
		if (m_Options.m_Shadows) {
			MICROLOG(L"render shadows");
			RenderShadowMap();
		}
		// clear buffers
		glClearColor(m_ClearColor[0],m_ClearColor[1],m_ClearColor[2],m_ClearColor[3]);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	}

	oglCheck();

	// render submitted patches and models
	MICROLOG(L"render patches");
	RenderPatches();
	oglCheck();

	MICROLOG(L"render models");
	RenderModels();
	oglCheck();

	if (m_Options.m_Shadows && !m_ShadowRendered) {
		MICROLOG(L"apply shadows");
		ApplyShadowMap();
		oglCheck();
	}
	m_ShadowRendered=true;

	// call on the transparency renderer to render all the transparent stuff
	MICROLOG(L"render transparent");
	RenderTransparentModels();
	oglCheck();

	// render water (note: we're assuming there's no transparent stuff over water...
	// we could also do this above render transparent if we assume there's no transparent
	// stuff underwater)
	if (m_WaterManager->m_RenderWater)
	{
		MICROLOG(L"render water");
		m->terrainRenderer->RenderWater();
		oglCheck();
	}

	// empty lists
	MICROLOG(L"empty lists");
	m->terrainRenderer->EndFrame();

	// Finish model renderers
	m_Models.NormalFF->EndFrame();
	m_Models.PlayerFF->EndFrame();
	m_Models.TranspFF->EndFrame();
	if (m_Models.NormalHWLit)
		m_Models.NormalHWLit->EndFrame();
	if (m_Models.PlayerHWLit)
		m_Models.PlayerHWLit->EndFrame();
	if (m_Models.TranspHWLit)
		m_Models.TranspHWLit->EndFrame();
	if (m_Models.NormalInstancing)
		m_Models.NormalInstancing->EndFrame();
	if (m_Models.PlayerInstancing)
		m_Models.PlayerInstancing->EndFrame();
	m_Models.TranspSortAll->EndFrame();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// EndFrame: signal frame end; implicitly flushes batched objects
void CRenderer::EndFrame()
{
#ifndef SCED
	if(!g_Game || !g_Game->IsGameStarted())
		return;
#endif

	FlushFrame();
	g_Renderer.SetTexture(0,0);

	static bool once=false;
	if (!once && glGetError()) {
		LOG(ERROR, LOG_CATEGORY, "CRenderer::EndFrame: GL errors occurred");
		once=true;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// SetCamera: setup projection and transform of camera and adjust viewport to current view
void CRenderer::SetCamera(CCamera& camera)
{
	CMatrix3D view;
	camera.m_Orientation.GetInverse(view);
	const CMatrix3D& proj=camera.GetProjection();

	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(&proj._11);

	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(&view._11);

	SetViewport(camera.GetViewPort());

	m_Camera=camera;
}

void CRenderer::SetViewport(const SViewPort &vp)
{
	glViewport(vp.m_X,vp.m_Y,vp.m_Width,vp.m_Height);
}

void CRenderer::Submit(CPatch* patch)
{
	m->terrainRenderer->Submit(patch);
}

void CRenderer::Submit(CModel* model)
{
	if (model->GetFlags() & MODELFLAG_CASTSHADOWS) {
		PROFILE( "updating shadow bounds" );
		m_ShadowBound += model->GetBounds();
	}

	// Tricky: The call to GetBounds() above can invalidate the position
	model->ValidatePosition();

	bool canUseInstancing = false;

	if (model->GetModelDef()->GetNumBones() == 0)
		canUseInstancing = true;

	if (model->GetMaterial().IsPlayer())
	{
		if (m_Options.m_RenderPath == RP_VERTEXSHADER)
		{
			if (canUseInstancing && m_Models.PlayerInstancing)
				m_Models.PlayerInstancing->Submit(model);
			else
				m_Models.PlayerHWLit->Submit(model);
		}
		else
			m_Models.PlayerFF->Submit(model);
	}
	else if (model->GetMaterial().UsesAlpha())
	{
		if (m_SortAllTransparent)
			m_Models.TranspSortAll->Submit(model);
		else if (m_Options.m_RenderPath == RP_VERTEXSHADER)
			m_Models.TranspHWLit->Submit(model);
		else
			m_Models.TranspFF->Submit(model);
	}
	else
	{
		if (m_Options.m_RenderPath == RP_VERTEXSHADER)
		{
			if (canUseInstancing && m_Models.NormalInstancing)
				m_Models.NormalInstancing->Submit(model);
			else
				m_Models.NormalHWLit->Submit(model);
		}
		else
			m_Models.NormalFF->Submit(model);
	}
}

void CRenderer::Submit(CSprite* UNUSED(sprite))
{
}

void CRenderer::Submit(CParticleSys* UNUSED(psys))
{
}

void CRenderer::Submit(COverlay* UNUSED(overlay))
{
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LoadTexture: try and load the given texture; set clamp/repeat flags on texture object if necessary
bool CRenderer::LoadTexture(CTexture* texture,u32 wrapflags)
{
	const Handle errorhandle = -1;

	Handle h=texture->GetHandle();
	// already tried to load this texture
	if (h)
	{
		// nothing to do here - just return success according to
		// whether this is a valid handle or not
		return h==errorhandle ? true : false;
	}

	h=ogl_tex_load(texture->GetName());
	if (h <= 0)
	{
		LOG(ERROR, LOG_CATEGORY, "LoadTexture failed on \"%s\"",(const char*) texture->GetName());
		texture->SetHandle(errorhandle);
		return false;
	}

	if(!wrapflags)
		wrapflags = GL_CLAMP_TO_EDGE;
	(void)ogl_tex_set_wrap(h, wrapflags);
	(void)ogl_tex_set_filter(h, GL_LINEAR_MIPMAP_LINEAR);

	// (this also verifies that the texture is a power-of-two)
	if(ogl_tex_upload(h) < 0)
	{
		LOG(ERROR, LOG_CATEGORY, "LoadTexture failed on \"%s\" : upload failed",(const char*) texture->GetName());
		ogl_tex_free(h);
		texture->SetHandle(errorhandle);
		return false;
	}

	texture->SetHandle(h);
	return true;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BindTexture: bind a GL texture object to current active unit
void CRenderer::BindTexture(int unit,GLuint tex)
{
	pglActiveTextureARB(GL_TEXTURE0+unit);

	glBindTexture(GL_TEXTURE_2D,tex);
	if (tex) {
		glEnable(GL_TEXTURE_2D);
	} else {
		glDisable(GL_TEXTURE_2D);
	}
	m_ActiveTextures[unit]=tex;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SetTexture: set the given unit to reference the given texture; pass a null texture to disable texturing on any unit
void CRenderer::SetTexture(int unit,CTexture* texture)
{
	Handle h = texture? texture->GetHandle() : 0;
	ogl_tex_bind(h, unit);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// IsTextureTransparent: return true if given texture is transparent, else false - note texture must be loaded
// beforehand
bool CRenderer::IsTextureTransparent(CTexture* texture)
{
	if (!texture) return false;
	Handle h=texture->GetHandle();

	uint flags = 0;	// assume no alpha on failure
	(void)ogl_tex_get_format(h, &flags, 0);
	return (flags & TEX_ALPHA) != 0;
}




static inline void CopyTriple(unsigned char* dst,const unsigned char* src)
{
	dst[0]=src[0];
	dst[1]=src[1];
	dst[2]=src[2];
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// LoadAlphaMaps: load the 14 default alpha maps, pack them into one composite texture and
// calculate the coordinate of each alphamap within this packed texture
int CRenderer::LoadAlphaMaps()
{
	const char* const key = "(alpha map composite)";
	Handle ht = ogl_tex_find(key);
	// alpha map texture had already been created and is still in memory:
	// reuse it, do not load again.
	if(ht > 0)
	{
		m_hCompositeAlphaMap = ht;
		return 0;
	}

	//
	// load all textures and store Handle in array
	//
	Handle textures[NumAlphaMaps] = {0};
	PathPackage pp;
	(void)pp_set_dir(&pp, "art/textures/terrain/alphamaps/special");
	const char* fnames[NumAlphaMaps] = {
		"blendcircle.dds",
		"blendlshape.dds",
		"blendedge.dds",
		"blendedgecorner.dds",
		"blendedgetwocorners.dds",
		"blendfourcorners.dds",
		"blendtwooppositecorners.dds",
		"blendlshapecorner.dds",
		"blendtwocorners.dds",
		"blendcorner.dds",
		"blendtwoedges.dds",
		"blendthreecorners.dds",
		"blendushape.dds",
		"blendbad.dds"
	};
	uint base = 0;	// texture width/height (see below)
	// for convenience, we require all alpha maps to be of the same BPP
	// (avoids another ogl_tex_get_size call, and doesn't hurt)
	uint bpp = 0;
	for(uint i=0;i<NumAlphaMaps;i++)
	{
		(void)pp_append_file(&pp, fnames[i]);
		// note: these individual textures can be discarded afterwards;
		// we cache the composite.
		textures[i] = ogl_tex_load(pp.path, RES_NO_CACHE);
		RETURN_ERR(textures[i]);

// quick hack: we require plain RGB(A) format, so convert to that.
// ideally the texture would be in uncompressed form; then this wouldn't
// be necessary.
uint flags;
ogl_tex_get_format(textures[i], &flags, 0);
ogl_tex_transform_to(textures[i], flags & ~TEX_DXT);

		// get its size and make sure they are all equal.
		// (the packing algo assumes this)
		uint this_width = 0, this_bpp = 0;	// fail-safe
		(void)ogl_tex_get_size(textures[i], &this_width, 0, &this_bpp);
		// .. first iteration: establish size
		if(i == 0)
		{
			base = this_width;
			bpp  = this_bpp;
		}
		// .. not first: make sure texture size matches
		else if(base != this_width || bpp != this_bpp)
			DISPLAY_ERROR(L"Alpha maps are not identically sized (including pixel depth)");
	}

	//
	// copy each alpha map (tile) into one buffer, arrayed horizontally.
	//
	uint tile_w = 2+base+2;	// 2 pixel border (avoids bilinear filtering artifacts)
	uint total_w = RoundUpToPowerOf2(tile_w * NumAlphaMaps);
	uint total_h = base; debug_assert(is_pow2(total_h));
	u8* data=new u8[total_w*total_h*3];
	// for each tile on row
	for(uint i=0;i<NumAlphaMaps;i++)
	{
		// get src of copy
		const u8* src = 0;
		(void)ogl_tex_get_data(textures[i], (void**)&src);

		uint srcstep=bpp/8;

		// get destination of copy
		u8* dst=data+3*(i*tile_w);

		// for each row of image
		for (uint j=0;j<base;j++) {
			// duplicate first pixel
			CopyTriple(dst,src);
			dst+=3;
			CopyTriple(dst,src);
			dst+=3;

			// copy a row
			for (uint k=0;k<base;k++) {
				CopyTriple(dst,src);
				dst+=3;
				src+=srcstep;
			}
			// duplicate last pixel
			CopyTriple(dst,(src-srcstep));
			dst+=3;
			CopyTriple(dst,(src-srcstep));
			dst+=3;

			// advance write pointer for next row
			dst+=3*(total_w-tile_w);
		}

		m_AlphaMapCoords[i].u0=float(i*tile_w+2)/float(total_w);
		m_AlphaMapCoords[i].u1=float((i+1)*tile_w-2)/float(total_w);
		m_AlphaMapCoords[i].v0=0.0f;
		m_AlphaMapCoords[i].v1=1.0f;
	}

	for (uint i=0;i<NumAlphaMaps;i++)
		(void)ogl_tex_free(textures[i]);

	// upload the composite texture
	Tex t;
	(void)tex_wrap(total_w, total_h, 24, 0, data, &t);
	m_hCompositeAlphaMap = ogl_tex_wrap(&t, key);
	(void)ogl_tex_set_filter(m_hCompositeAlphaMap, GL_LINEAR);
	(void)ogl_tex_set_wrap  (m_hCompositeAlphaMap, GL_CLAMP_TO_EDGE);
	int ret = ogl_tex_upload(m_hCompositeAlphaMap, 0, 0, GL_INTENSITY);
	delete[] data;

	return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// UnloadAlphaMaps: frees the resources allocates by LoadAlphaMaps
void CRenderer::UnloadAlphaMaps()
{
	ogl_tex_free(m_hCompositeAlphaMap);
	m_hCompositeAlphaMap = 0;
}



///////////////////////////////////////////////////////////////////////////////////////////////////
// Scripting Interface

jsval CRenderer::JSI_GetFastPlayerColor(JSContext*)
{
	return ToJSVal(m_FastPlayerColor);
}

void CRenderer::JSI_SetFastPlayerColor(JSContext* ctx, jsval newval)
{
	bool fast;

	if (!ToPrimitive(ctx, newval, fast))
		return;

	SetFastPlayerColor(fast);
}

jsval CRenderer::JSI_GetRenderPath(JSContext*)
{
	return ToJSVal(GetRenderPathName(m_Options.m_RenderPath));
}

void CRenderer::JSI_SetRenderPath(JSContext* ctx, jsval newval)
{
	CStr name;

	if (!ToPrimitive(ctx, newval, name))
		return;

	SetRenderPath(GetRenderPathByName(name));
}

void CRenderer::ScriptingInit()
{
	AddProperty(L"fastPlayerColor", &CRenderer::JSI_GetFastPlayerColor, &CRenderer::JSI_SetFastPlayerColor);
	AddProperty(L"renderpath", &CRenderer::JSI_GetRenderPath, &CRenderer::JSI_SetRenderPath);
	AddProperty(L"sortAllTransparent", &CRenderer::m_SortAllTransparent);
	AddProperty(L"fastNormals", &CRenderer::m_FastNormals);

	CJSObject<CRenderer>::ScriptingInit("Renderer");
}

