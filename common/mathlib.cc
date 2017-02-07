/*  Copyright (C) 1996-1997  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <common/cmdlib.hh>
#include <common/mathlib.hh>
#include <assert.h>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

const vec3_t vec3_origin = { 0, 0, 0 };

qboolean
VectorCompare(const vec3_t v1, const vec3_t v2)
{
    int i;

    for (i = 0; i < 3; i++)
        if (fabs(v1[i] - v2[i]) > EQUAL_EPSILON)
            return false;

    return true;
}

void
CrossProduct(const vec3_t v1, const vec3_t v2, vec3_t cross)
{
    cross[0] = v1[1] * v2[2] - v1[2] * v2[1];
    cross[1] = v1[2] * v2[0] - v1[0] * v2[2];
    cross[2] = v1[0] * v2[1] - v1[1] * v2[0];
}

/*
 * VecStr - handy shortcut for printf, not thread safe, obviously
 */
const char *
VecStr(const vec3_t vec)
{
    static char buffers[8][20];
    static int current = 0;
    char *buf;

    buf = buffers[current++ & 7];
    q_snprintf(buf, sizeof(buffers[0]), "%i %i %i",
             (int)vec[0], (int)vec[1], (int)vec[2]);

    return buf;
}

const char *
VecStrf(const vec3_t vec)
{
    static char buffers[8][20];
    static int current = 0;
    char *buf;

    buf = buffers[current++ & 7];
    q_snprintf(buf, sizeof(buffers[0]), "%.2f %.2f %.2f",
             vec[0], vec[1], vec[2]);

    return buf;
}

// from http://mathworld.wolfram.com/SpherePointPicking.html
// eqns 6,7,8
void
UniformPointOnSphere(vec3_t dir, float u1, float u2)
{
    Q_assert(u1 >= 0 && u1 <= 1);
    Q_assert(u2 >= 0 && u2 <= 1);
    
    const vec_t theta = u1 * 2.0 * Q_PI;
    const vec_t u = (2.0 * u2) - 1.0;
    
    const vec_t s = sqrt(1.0 - (u * u));
    dir[0] = s * cos(theta);
    dir[1] = s * sin(theta);
    dir[2] = u;
    
    for (int i=0; i<3; i++) {
        Q_assert(dir[i] >= -1.001);
        Q_assert(dir[i] <=  1.001);
    }
}

void
RandomDir(vec3_t dir)
{
    UniformPointOnSphere(dir, Random(), Random());
}


bool AABBsDisjoint(const vec3_t minsA, const vec3_t maxsA,
                   const vec3_t minsB, const vec3_t maxsB)
{
    for (int i=0; i<3; i++) {
        if (maxsA[i] < (minsB[i] - EQUAL_EPSILON)) return true;
        if (minsA[i] > (maxsB[i] + EQUAL_EPSILON)) return true;
    }
    return false;
}

void AABB_Init(vec3_t mins, vec3_t maxs, const vec3_t pt) {
    VectorCopy(pt, mins);
    VectorCopy(pt, maxs);
}

void AABB_Expand(vec3_t mins, vec3_t maxs, const vec3_t pt) {
    for (int i=0; i<3; i++) {
        mins[i] = qmin(mins[i], pt[i]);
        maxs[i] = qmax(maxs[i], pt[i]);
    }
}

void AABB_Size(const vec3_t mins, const vec3_t maxs, vec3_t size_out) {
    for (int i=0; i<3; i++) {
        size_out[i] = maxs[i] - mins[i];
    }
}

void AABB_Grow(vec3_t mins, vec3_t maxs, const vec3_t size) {
    for (int i=0; i<3; i++) {
        mins[i] -= size[i];
        maxs[i] += size[i];
    }
}

glm::vec2 Barycentric_FromPoint(const glm::vec3 &p, const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c)
{
    const glm::vec3 v0 = b - a;
    const glm::vec3 v1 = c - a;
    const glm::vec3 v2 = p - a;
    float d00 = glm::dot(v0, v0);
    float d01 = glm::dot(v0, v1);
    float d11 = glm::dot(v1, v1);
    float d20 = glm::dot(v2, v0);
    float d21 = glm::dot(v2, v1);
    float invDenom = (d00 * d11 - d01 * d01);
    invDenom = 1.0/invDenom;
    
    glm::vec2 res((d11 * d20 - d01 * d21) * invDenom,
                  (d00 * d21 - d01 * d20) * invDenom);
    return res;
}

// from global illumination total compendium p. 12
glm::vec2 Barycentric_Random(const float r1, const float r2)
{
    glm::vec2 res(1.0f - sqrtf(r1),
                  r2 * sqrtf(r1));
    return res;
}

/// Evaluates the given barycentric coord for the given triangle
glm::vec3 Barycentric_ToPoint(const glm::vec2 &bary,
                              const glm::vec3 &a,
                              const glm::vec3 &b,
                              const glm::vec3 &c)
{
    const glm::vec3 pt = a + (bary.s * (b - a)) + (bary.t * (c - a));
    return pt;
}


vec_t
TriangleArea(const vec3_t v0, const vec3_t v1, const vec3_t v2)
{
    vec3_t edge0, edge1, cross;
    VectorSubtract(v2, v0, edge0);
    VectorSubtract(v1, v0, edge1);
    CrossProduct(edge0, edge1, cross);
    
    return VectorLength(cross) * 0.5;
}

static std::vector<float>
NormalizePDF(const std::vector<float> &pdf)
{
    float pdfSum = 0.0f;
    for (float val : pdf) {
        pdfSum += val;
    }
    
    std::vector<float> normalizedPdf;
    for (float val : pdf) {
        normalizedPdf.push_back(val / pdfSum);
    }
    return normalizedPdf;
}

std::vector<float> MakeCDF(const std::vector<float> &pdf)
{
    const std::vector<float> normzliedPdf = NormalizePDF(pdf);
    std::vector<float> cdf;
    float cdfSum = 0.0f;
    for (float val : normzliedPdf) {
        cdfSum += val;
        cdf.push_back(cdfSum);
    }
    return cdf;
}

int SampleCDF(const std::vector<float> &cdf, float sample)
{
    const size_t size = cdf.size();
    for (size_t i=0; i<size; i++) {
        float cdfVal = cdf.at(i);
        if (sample <= cdfVal) {
            return i;
        }
    }
    Q_assert_unreachable();
    return 0;
}

static float Gaussian1D(float width, float x, float alpha)
{
    if (fabs(x) > width)
        return 0.0f;
    
    return expf(-alpha * x * x) - expf(-alpha * width * width);
}

float Filter_Gaussian(float width, float height, float x, float y)
{
    const float alpha = 0.5;
    return Gaussian1D(width, x, alpha)
        * Gaussian1D(height, y, alpha);
}

// from https://en.wikipedia.org/wiki/Lanczos_resampling
static float Lanczos1D(float x, float a)
{
    if (x == 0)
        return 1;
    
    if (x < -a || x >= a)
        return 0;
    
    float lanczos = (a * sinf(M_PI * x) * sinf(M_PI * x / a)) / (M_PI * M_PI * x * x);
    return lanczos;
}

// from https://en.wikipedia.org/wiki/Lanczos_resampling#Multidimensional_interpolation
float Lanczos2D(float x, float y, float a)
{
    float dist = sqrtf((x*x) + (y*y));
    float lanczos = Lanczos1D(dist, a);
    return lanczos;
}

using namespace glm;
using namespace std;

glm::vec3 GLM_FaceNormal(std::vector<glm::vec3> points)
{
    const int N = static_cast<int>(points.size());
    float maxArea = -FLT_MAX;
    int bestI = -1;
    
    const vec3 p0 = points[0];
    
    for (int i=2; i<N; i++) {
        const vec3 p1 = points[i-1];
        const vec3 p2 = points[i];
        
        const float area = GLM_TriangleArea(p0, p1, p2);
        if (area > maxArea) {
            maxArea = area;
            bestI = i;
        }
    }
    
    if (bestI == -1 || maxArea < ZERO_TRI_AREA_EPSILON)
        return vec3(0);
    
    const vec3 p1 = points[bestI-1];
    const vec3 p2 = points[bestI];
    const vec3 normal = normalize(cross(p2 - p0, p1 - p0));
    return normal;
}

vector<vec4>
GLM_MakeInwardFacingEdgePlanes(std::vector<vec3> points)
{
    if (points.size() < 3)
        return {};
    
    vector<vec4> result;
    const vec3 faceNormal = GLM_FaceNormal(points);
    
    if (faceNormal == vec3(0,0,0))
        return {};
    
    for (int i=0; i<points.size(); i++)
    {
        const vec3 v0 = points.at(i);
        const vec3 v1 = points.at((i+1) % points.size());
        
        const float v0v1len = length(v1-v0);
        if (v0v1len < POINT_EQUAL_EPSILON)
            continue;
        
        const vec3 edgedir = (v1 - v0) / v0v1len;
        const vec3 edgeplane_normal = cross(edgedir, faceNormal);
        const float edgeplane_dist = dot(edgeplane_normal, v0);
        
        result.push_back(vec4(edgeplane_normal, edgeplane_dist));
    }
    
    return result;
}

float GLM_EdgePlanes_PointInsideDist(const std::vector<glm::vec4> &edgeplanes, const glm::vec3 &point)
{
    float min = FLT_MAX;
    
    for (int i=0; i<edgeplanes.size(); i++) {
        const float planedist = GLM_DistAbovePlane(edgeplanes[i], point);
        if (planedist < min)
            min = planedist;
    }
    
    return min; // "outermost" point
}

bool
GLM_EdgePlanes_PointInside(const vector<vec4> &edgeplanes, const vec3 &point)
{
    if (edgeplanes.empty())
        return false;
    
    const float minDist = GLM_EdgePlanes_PointInsideDist(edgeplanes, point);
    return minDist >= -POINT_EQUAL_EPSILON;
}

float
GLM_TriangleArea(const vec3 &v0, const vec3 &v1, const vec3 &v2)
{
    return 0.5f * length(cross(v2 - v0, v1 - v0));
}

float GLM_DistAbovePlane(const glm::vec4 &plane, const glm::vec3 &point)
{
    return dot(vec3(plane), point) - plane.w;
}
