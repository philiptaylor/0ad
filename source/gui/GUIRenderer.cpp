#include "precompiled.h"

#include "GUIRenderer.h"

#include "lib/ogl.h"
#include "lib/res/h_mgr.h"

#include "ps/CLogger.h"
#define LOG_CATEGORY "gui"

using namespace GUIRenderer;


void DrawCalls::clear()
{
	for (iterator it = begin(); it != end(); ++it)
	{
		delete it->m_Effects;
		tex_free(it->m_TexHandle);
	}
	std::vector<SDrawCall>::clear();
}

DrawCalls::DrawCalls()
{
}

DrawCalls::~DrawCalls()
{
	clear();
	std::vector<SDrawCall>::~vector();
}

// Never copy anything (to avoid losing track of who owns various pointers):

DrawCalls::DrawCalls(const DrawCalls&)
{
}

const DrawCalls& DrawCalls::operator=(const DrawCalls&)
{
	return *this;
}



// Implementations of graphical effects

class Effect_AddColor : public IGLState
{
public:
	Effect_AddColor(CColor c) : m_Color(c) {}
	~Effect_AddColor() {}
	void Set(Handle tex)
	{
		glColor4fv(m_Color.FloatArray());
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
		tex_bind(tex);
	}
	void Unset()
	{
	}
private:
	CColor m_Color;
};

class Effect_MultiplyColor : public IGLState
{
public:
	Effect_MultiplyColor(CColor c) : m_Color(c) {}
	~Effect_MultiplyColor() {}
	void Set(Handle tex)
	{
		glColor4fv(m_Color.FloatArray());
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		tex_bind(tex);
	}
	void Unset()
	{
	}
private:
	CColor m_Color;
};

#define X(n) (n##f/2.0f + 0.5f)
const float GreyscaleDotColor[4] = { X(0.3), X(0.59), X(0.11), 1.0f };
#undef X
const float GreyscaleInterpColor0[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
const float GreyscaleInterpColor1[4] = { 0.5f, 0.5f, 0.5f, 1.0f };

class Effect_Greyscale : public IGLState
{
public:
	~Effect_Greyscale() {}
	void Set(Handle tex)
	{
		/*

		For the main conversion, use GL_DOT3_RGB, which is defined as
		    L = 4 * ((Arg0r - 0.5) * (Arg1r - 0.5)+
		             (Arg0g - 0.5) * (Arg1g - 0.5)+
			         (Arg0b - 0.5) * (Arg1b - 0.5))
		where each of the RGB components is given the value 'L'.

		Use the magical luminance formula
		    L = 0.3R + 0.59G + 0.11B
		to calculate the greyscale value.

		But to work around the annoying "Arg0-0.5", we need to calculate
		Arg0+0.5. But we also need to scale it into the range 0.5-1.0, else
		Arg0>0.5 will be clamped to 1.0. So use GL_INTERPOLATE, which outputs:
		    A0 * A2 + A1 * (1 - A2)
		and set A2 = 0.5, A1 = 1.0, and A0 = texture (i.e. interpolating halfway
		between the texture and {1,1,1}) giving
		    A0/2 + 0.5
		and use that as Arg0.
		
		So L = 4*(A0/2 * (Arg1-.5))
		     = 2 (Rx+Gy+Bz)      (where Arg1 = {x+0.5, y+0.5, z+0.5})
			 = 2x R + 2y G + 2z B
			 = 0.3R + 0.59G + 0.11B
		so e.g. 2y = 0.59 = 2(Arg1g-0.5) => Arg1g = 0.59/2+0.5
		which fortunately doesn't get clamped.

		So, just implement that:

		*/

		// TODO: Render all greyscale objects at the same time, to reduce
		// the number of times the following code is called - it looks like
		// a rather worrying amount of work for rendering a single button...

		// Texture unit 0:

		glEnable(GL_TEXTURE_2D);
		tex_bind(tex);

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);

		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, GreyscaleInterpColor0);

		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_COLOR);
		glColor4fv(GreyscaleInterpColor1);

		// Texture unit 1:

		glActiveTexture(GL_TEXTURE1);
		glEnable(GL_TEXTURE_2D);
		tex_bind(tex);

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_DOT3_RGB);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);

		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, GreyscaleDotColor);

	}
	void Unset()
	{
		glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE0);
	}
};



// Functions to perform drawing-related actions:

void GUIRenderer::UpdateDrawCallCache(DrawCalls &Calls, CStr &SpriteName, CRect &Size, int CellID, std::map<CStr, CGUISprite> &Sprites)
{
	// This is called only when something has changed (like the size of the
	// sprite), so it doesn't need to be particularly efficient.

	// Clean up the old data
	Calls.clear();

	std::map<CStr, CGUISprite>::iterator it (Sprites.find(SpriteName));
	if (it == Sprites.end())
	{
		// Sprite not found. Check whether this a special sprite:
		//     stretched:filename.ext
		//     <currently that's the only one>
		// and if so, try to create it as a new sprite.
		if (SpriteName.substr(0, 10) == "stretched:")
		{
			SGUIImage Image;
			Image.m_TextureName = "art/textures/ui/" + SpriteName.substr(10);
			CClientArea ca("0 0 100% 100%");
			Image.m_Size = ca;
			Image.m_TextureSize = ca;

			CGUISprite Sprite;
			Sprite.AddImage(Image);

			Sprites[SpriteName] = Sprite;
			
			it = Sprites.find(SpriteName);
			assert(it != Sprites.end()); // The insertion above shouldn't fail
		}
		else
		{
			// Otherwise, just complain and give up:
			LOG(ERROR, LOG_CATEGORY, "Trying to use a sprite that doesn't exist (\"%s\").", (const char*)SpriteName);
			return;
		}
	}

    Calls.reserve(it->second.m_Images.size());

	// Iterate through all the sprite's images, loading the texture and
	// calculating the texture coordinates
	std::vector<SGUIImage>::const_iterator cit;
	for (cit = it->second.m_Images.begin(); cit != it->second.m_Images.end(); ++cit)
	{
		SDrawCall Call;

		CRect ObjectSize = cit->m_Size.GetClientArea(Size);
		Call.m_Vertices = ObjectSize;

		if (cit->m_TextureName.Length())
		{
			Handle h = tex_load(cit->m_TextureName);
			if (h <= 0)
			{
				LOG(ERROR, LOG_CATEGORY, "Error reading texture '%s': %lld", (const char*)cit->m_TextureName, h);
				return;
			}

			int err = tex_upload(h);
			if (err < 0)
			{
				LOG(ERROR, LOG_CATEGORY, "Error uploading texture '%s': %d", (const char*)cit->m_TextureName, err);
				return;
			}

			Call.m_TexHandle = h;

			int TexFormat, t_w, t_h;
			tex_info(h, &t_w, &t_h, &TexFormat, NULL, NULL);
			float TexWidth = (float)t_w, TexHeight = (float)t_h;
			
			// TODO: Detect the presence of an alpha channel in a nicer way
			Call.m_EnableBlending = (TexFormat == GL_RGBA || TexFormat == GL_BGRA);


			// Textures are positioned by defining a rectangular block of the
			// texture (usually the whole texture), and a rectangular block on
			// the screen. The texture is positioned to make those blocks line up.

			
			// Get the screen's position/size for the block
			CRect BlockScreen = cit->m_TextureSize.GetClientArea(ObjectSize);


			// Get the texture's position/size for the block:
			CRect BlockTex;

			// "real-texture-placement" overrides everything
			if (cit->m_TexturePlacementInFile != CRect())
				BlockTex = cit->m_TexturePlacementInFile;

			// Check whether this sprite has "cell-size" set
			else if (cit->m_CellSize != CSize())
			{
				int cols = t_w / (int)cit->m_CellSize.cx;
				int col = CellID % cols;
				int row = CellID / cols;
				BlockTex = CRect(cit->m_CellSize.cx*col, cit->m_CellSize.cy*row,
								 cit->m_CellSize.cx*(col+1), cit->m_CellSize.cy*(row+1));
			}

			// Use the whole texture
			else
				BlockTex = CRect(0, 0, TexWidth, TexHeight);


			// When rendering, BlockTex will be transformed onto BlockScreen.
			// Also, TexCoords will be transformed onto ObjectSize (giving the
			// UV coords at each vertex of the object). We know everything
			// except for TexCoords, so calculate it:

			CPos translation (BlockTex.TopLeft()-BlockScreen.TopLeft());
			float ScaleW = BlockTex.GetWidth()/BlockScreen.GetWidth();
			float ScaleH = BlockTex.GetHeight()/BlockScreen.GetHeight();
			
			CRect TexCoords (
						// Resize (translating to/from the origin, so the
						// topleft corner stays in the same place)
						(ObjectSize-ObjectSize.TopLeft())
						.Scale(ScaleW, ScaleH)
						+ ObjectSize.TopLeft()
						// Translate from BlockTex to BlockScreen
						+ translation
			);

			// The tex coords need to be scaled so that (texwidth,texheight) is
			// mapped onto (1,1)
			TexCoords.left   /= TexWidth;
			TexCoords.right  /= TexWidth;
			// and flip it vertically, because of some confusion between coordinate systems
			TexCoords.top    /= -TexHeight;
			TexCoords.bottom /= -TexHeight;

			Call.m_TexCoords = TexCoords;
		}
		else
		{
			Call.m_TexHandle = 0;
			// Enable blending if it's transparent (allowing a little error in the calculations)
			Call.m_EnableBlending = !(fabs(cit->m_BackColor.a - 1.0f) < 0.0000001f);
		}

		Call.m_BackColor = cit->m_BackColor;
		Call.m_BorderColor = cit->m_Border ? cit->m_BorderColor : CColor();
		Call.m_DeltaZ = cit->m_DeltaZ;
		
		if (cit->m_Effects)
		{
			if (cit->m_Effects->m_AddColor != CColor())
				Call.m_Effects = new Effect_AddColor(cit->m_Effects->m_AddColor);
			
			else if (cit->m_Effects->m_MultiplyColor != CColor())
				Call.m_Effects = new Effect_MultiplyColor(cit->m_Effects->m_MultiplyColor);

			else if (cit->m_Effects->m_Greyscale)
				Call.m_Effects = new Effect_Greyscale;

			else
				/* Slight confusion - why no effects? */
				Call.m_Effects = NULL;
		}
		else
		{
			Call.m_Effects = NULL;

/* TODO: Delete	this code
			_CrtMemState s;
			_CrtMemCheckpoint(&s);
			struct ::_CrtMemBlockHeader
			{
				struct _CrtMemBlockHeader * pBlockHeaderNext;
				struct _CrtMemBlockHeader * pBlockHeaderPrev;
				char *                      szFileName;
				int                         nLine;
				size_t                      nDataSize;
				int                         nBlockUse;
				long                        lRequest;
			};
			debug_out("%d %s\n", s.pBlockHeader->lRequest, SpriteName.c_str());
*/
		}

		Calls.push_back(Call);
	}
}

void GUIRenderer::Draw(DrawCalls &Calls)
{
	// Called every frame, to draw the object (based on cached calculations)


	// Iterate through each DrawCall, and execute whatever drawing code is being called
	for (DrawCalls::const_iterator cit = Calls.begin(); cit != Calls.end(); ++cit)
	{
		if (cit->m_EnableBlending)
		{
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glEnable(GL_BLEND);
		}

		if (cit->m_TexHandle)
		{
			// TODO: Handle the GL state in a nicer way

			if (cit->m_Effects)
				cit->m_Effects->Set(cit->m_TexHandle);
			else
			{
				glEnable(GL_TEXTURE_2D);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
				tex_bind(cit->m_TexHandle);
			}
			
			glBegin(GL_QUADS);

				glTexCoord2f(cit->m_TexCoords.right,cit->m_TexCoords.bottom);
				glVertex3f(cit->m_Vertices.right,	cit->m_Vertices.bottom,	cit->m_DeltaZ);

				glTexCoord2f(cit->m_TexCoords.left,	cit->m_TexCoords.bottom);
				glVertex3f(cit->m_Vertices.left,	cit->m_Vertices.bottom,	cit->m_DeltaZ);

				glTexCoord2f(cit->m_TexCoords.left,	cit->m_TexCoords.top);
				glVertex3f(cit->m_Vertices.left,	cit->m_Vertices.top,	cit->m_DeltaZ);

				glTexCoord2f(cit->m_TexCoords.right,cit->m_TexCoords.top);
				glVertex3f(cit->m_Vertices.right,	cit->m_Vertices.top,	cit->m_DeltaZ);

			glEnd();

			if (cit->m_Effects)
				cit->m_Effects->Unset();
		}
		else
		{
			glDisable(GL_TEXTURE_2D);

			glColor4fv(cit->m_BackColor.FloatArray());

			glBegin(GL_QUADS);
				glVertex3f(cit->m_Vertices.right,	cit->m_Vertices.bottom,	cit->m_DeltaZ);
				glVertex3f(cit->m_Vertices.left,	cit->m_Vertices.bottom,	cit->m_DeltaZ);
				glVertex3f(cit->m_Vertices.left,	cit->m_Vertices.top,	cit->m_DeltaZ);
				glVertex3f(cit->m_Vertices.right,	cit->m_Vertices.top,	cit->m_DeltaZ);
			glEnd();


			if (cit->m_BorderColor != CColor())
			{
				glColor4fv(cit->m_BorderColor.FloatArray());
				glBegin(GL_LINE_LOOP);
					glVertex3f(cit->m_Vertices.left,		cit->m_Vertices.top+1.f,	cit->m_DeltaZ);
					glVertex3f(cit->m_Vertices.right-1.f,	cit->m_Vertices.top+1.f,	cit->m_DeltaZ);
					glVertex3f(cit->m_Vertices.right-1.f,	cit->m_Vertices.bottom,		cit->m_DeltaZ);
					glVertex3f(cit->m_Vertices.left,		cit->m_Vertices.bottom,		cit->m_DeltaZ);
				glEnd();
			}
		}

		if (cit->m_EnableBlending)
		{
			glDisable(GL_BLEND);
		}

	}
}
