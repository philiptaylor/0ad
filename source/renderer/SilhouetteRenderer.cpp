/* Copyright (C) 2014 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "SilhouetteRenderer.h"

#include "graphics/Camera.h"
#include "graphics/Model.h"
#include "graphics/ShaderManager.h"
#include "maths/MathUtil.h"
#include "ps/Profile.h"
#include "renderer/Renderer.h"
#include "renderer/Scene.h"

SilhouetteRenderer::SilhouetteRenderer()
{
	m_DebugEnabled = false;
}

void SilhouetteRenderer::AddOccluder(CPatch* patch)
{
	m_SubmittedPatchOccluders.push_back(patch);
}

void SilhouetteRenderer::AddOccluder(CModel* model)
{
	m_SubmittedModelOccluders.push_back(model);
}

void SilhouetteRenderer::AddDisplayer(CModel* model)
{
	m_SubmittedModelDisplayers.push_back(model);
}

struct Occluder
{
	CModel* model;
	u16 x0, y0, x1, y1;
	float z;
	bool rendered;
};

struct Displayer
{
	CModel* model;
	u16 x, y;
	float z;
	bool rendered;
};

struct Entry
{
	enum { RECT_IN, RECT_OUT, POINT };
	u16 x;
	u16 id;
	u8 type;
};

struct EntryCompare
{
	bool operator()(const Entry& a, const Entry& b)
	{
		if (a.x < b.x)
			return true;
		if (b.x < a.x)
			return false;

		// RECT_IN must sort before RECT_OUT
		if (a.type < b.type)
			return true;
		// Otherwise don't care about a stable order
		return false;
	}
};

struct ActiveList
{
	std::vector<u16> m_Ids;

	void Add(u16 id)
	{
		m_Ids.push_back(id);
	}

	void Remove(u16 id)
	{
		ssize_t sz = m_Ids.size();
		for (ssize_t i = sz-1; i >= 0; --i)
		{
			if (m_Ids[i] == id)
			{
				m_Ids[i] = m_Ids[sz-1];
				m_Ids.pop_back();
				return;
			}
		}
		debug_warn(L"Failed to find id");
	}
};

void SilhouetteRenderer::ComputeSubmissions(const CCamera& camera)
{
	PROFILE3("compute silhouettes");

	m_DebugBounds.clear();
	m_DebugRects.clear();

	m_VisiblePatchOccluders.clear();
	m_VisibleModelOccluders.clear();
	m_VisibleModelDisplayers.clear();

	std::vector<Occluder> occluders;
	std::vector<Displayer> displayers;
	std::vector<Entry> entries;

	CMatrix3D proj = camera.GetViewProjection();

	for (size_t i = 0; i < m_SubmittedModelOccluders.size(); ++i)
	{
		CModel* occluder = m_SubmittedModelOccluders[i];

		CBoundingBoxAligned bounds = occluder->GetWorldBounds();

		int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
		float z0 = FLT_MAX;
		for (size_t ix = 0; ix <= 1; ix++)
			for (size_t iy = 0; iy <= 1; iy++)
				for (size_t iz = 0; iz <= 1; iz++)
				{
					CVector4D vec(bounds[ix].X, bounds[iy].Y, bounds[iz].Z, 1.0f);
					CVector4D svec = proj.Transform(vec);
					x0 = std::min(x0, 2048 + (int)(2048.f * svec.X / svec.W));
					y0 = std::min(y0, 2048 + (int)(2048.f * svec.Y / svec.W));
					x1 = std::max(x1, 2048 + (int)(2048.f * svec.X / svec.W));
					y1 = std::max(y1, 2048 + (int)(2048.f * svec.Y / svec.W));
					z0 = std::min(z0, svec.Z / svec.W);
				}

		// XXX cull offscreen ones? (but there shouldn't be any)

		Occluder d;
		d.model = occluder;
		d.x0 = clamp(x0, 0, 4095);
		d.y0 = clamp(y0, 0, 4095);
		d.x1 = clamp(x1, 0, 4095);
		d.y1 = clamp(y1, 0, 4095);
		d.z = z0;
		d.rendered = false;
		size_t id = occluders.size();
		occluders.push_back(d);

		Entry e0, e1;
		e0.x = d.x0;
		e0.id = id;
		e0.type = Entry::RECT_IN;
		e1.x = d.x1;
		e1.id = id;
		e1.type = Entry::RECT_OUT;
		entries.push_back(e0);
		entries.push_back(e1);
	}

	for (size_t i = 0; i < m_SubmittedModelDisplayers.size(); ++i)
	{
		CModel* model = m_SubmittedModelDisplayers[i];
		CVector3D pos = model->GetTransform().GetTranslation();
		CVector4D vec(pos.X, pos.Y, pos.Z, 1.0f);
		CVector4D svec = proj.Transform(vec);
		int x = 2048 + (int)(2048.f * svec.X / svec.W);
		int y = 2048 + (int)(2048.f * svec.Y / svec.W);
		float z = svec.Z / svec.W;

		Displayer d;
		d.model = model;
		d.x = clamp(x, 0, 4095);
		d.y = clamp(y, 0, 4095);
		d.z = z;
		d.rendered = false;
		size_t id = displayers.size();
		displayers.push_back(d);

		Entry e;
		e.x = d.x;
		e.id = id;
		e.type = Entry::POINT;
		entries.push_back(e);
	}

	std::sort(entries.begin(), entries.end(), EntryCompare());

	ActiveList active;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		Entry e = entries[i];
		if (e.type == Entry::RECT_IN)
			active.Add(e.id);
		else if (e.type == Entry::RECT_OUT)
			active.Remove(e.id);
		else
		{
			Displayer& displayer = displayers[e.id];
			for (size_t j = 0; j < active.m_Ids.size(); ++j)
			{
				Occluder& occluder = occluders[active.m_Ids[j]];

				if (displayer.y < occluder.y0 || displayer.y > occluder.y1)
					continue;

				if (displayer.z < occluder.z)
					continue;

				displayer.rendered = true;
				occluder.rendered = true;
			}
		}
	}

	if (m_DebugEnabled)
	{
		for (size_t i = 0; i < occluders.size(); ++i)
		{
			DebugRect r;
			r.color = occluders[i].rendered ? CColor(1.0f, 1.0f, 0.0f, 1.0f) : CColor(0.2f, 0.2f, 0.0f, 1.0f);
			r.x0 = occluders[i].x0;
			r.y0 = occluders[i].y0;
			r.x1 = occluders[i].x1;
			r.y1 = occluders[i].y1;
			m_DebugRects.push_back(r);
		}
	}

	for (size_t i = 0; i < occluders.size(); ++i)
		if (occluders[i].rendered)
			m_VisibleModelOccluders.push_back(occluders[i].model);

	for (size_t i = 0; i < displayers.size(); ++i)
		if (displayers[i].rendered)
			m_VisibleModelDisplayers.push_back(displayers[i].model);
}

void SilhouetteRenderer::RenderSubmitOccluders(SceneCollector& collector)
{
	for (size_t i = 0; i < m_VisiblePatchOccluders.size(); ++i)
		collector.Submit(m_VisiblePatchOccluders[i]);

	for (size_t i = 0; i < m_VisibleModelOccluders.size(); ++i)
		collector.SubmitNonRecursive(m_VisibleModelOccluders[i]);
}

void SilhouetteRenderer::RenderSubmitDisplayers(SceneCollector& collector)
{
	for (size_t i = 0; i < m_VisibleModelDisplayers.size(); ++i)
		collector.SubmitNonRecursive(m_VisibleModelDisplayers[i]);
}

void SilhouetteRenderer::RenderDebugOverlays(const CCamera& camera)
{
	CShaderTechniquePtr shaderTech = g_Renderer.GetShaderManager().LoadEffect(str_gui_solid);
	shaderTech->BeginPass();
	CShaderProgramPtr shader = shaderTech->GetShader();

	glDepthMask(0);
	glDisable(GL_CULL_FACE);

	// Render various shadow bounds:
	//  Yellow = bounds of objects in view frustum that receive shadows
	//  Red = culling frustum used to find potential shadow casters
	//  Green = bounds of objects in culling frustum that cast shadows
	//  Blue = frustum used for rendering the shadow map

	shader->Uniform(str_transform, camera.GetViewProjection());

	for (size_t i = 0; i < m_DebugBounds.size(); ++i)
	{
		shader->Uniform(str_color, m_DebugBounds[i].color);
		m_DebugBounds[i].bounds.RenderOutline(shader);
	}

	CMatrix3D m;
	m.SetIdentity();
	m.Scale(1.0f, -1.f, 1.0f);
	m.Translate(0.0f, (float)g_yres, -1000.0f);

	CMatrix3D proj;
	proj.SetOrtho(0.f, 4096.f, 0.f, 4096.f, -1.f, 1000.f);
	m = proj * m;

	shader->Uniform(str_transform, proj);

	for (size_t i = 0; i < m_DebugRects.size(); ++i)
	{
		const DebugRect& r = m_DebugRects[i];
		shader->Uniform(str_color, r.color);
		u16 verts[] = {
			r.x0, r.y0,
			r.x1, r.y0,
			r.x1, r.y1,
			r.x0, r.y1,
			r.x0, r.y0,
		};
		shader->VertexPointer(2, GL_SHORT, 0, verts);
		glDrawArrays(GL_LINE_STRIP, 0, 5);
	}

	shaderTech->EndPass();

	glEnable(GL_CULL_FACE);
	glDepthMask(1);
}

void SilhouetteRenderer::EndFrame()
{
	m_SubmittedPatchOccluders.clear();
	m_SubmittedModelOccluders.clear();
	m_SubmittedModelDisplayers.clear();
}
