// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "patch.h"
#include "catmullclark_patch.h"
#include "bspline_patch.h"
#include "gregory_patch.h"
#include "tessellation.h"

namespace embree
{
  namespace isa {
  struct FeatureAdaptiveEval2
  {
    const size_t x0,x1;
    const size_t y0,y1;
    const size_t swidth,sheight;
    const float rcp_swidth, rcp_sheight;
    float* const Px;
    float* const Py;
    float* const Pz;
    float* const U;
    float* const V;
    const size_t dwidth,dheight;
    size_t count;
    
    typedef BilinearPatch3fa BilinearPatch;
    typedef BSplinePatch3fa BSplinePatch;
    typedef BezierPatch3fa BezierPatch;
    typedef GregoryPatch3fa GregoryPatch;
    
    __forceinline FeatureAdaptiveEval2 (const GeneralCatmullClarkPatch3fa& patch, size_t subPatch,
                                        const size_t x0, const size_t x1, const size_t y0, const size_t y1, const size_t swidth, const size_t sheight, 
                                        float* Px, float* Py, float* Pz, float* U, float* V, const size_t dwidth, const size_t dheight)
      : x0(x0), x1(x1), y0(y0), y1(y1), swidth(swidth), sheight(sheight), rcp_swidth(1.0f/(swidth-1.0f)), rcp_sheight(1.0f/(sheight-1.0f)), Px(Px), Py(Py), Pz(Pz), U(U), V(V), dwidth(dwidth), dheight(dheight), count(0)
    {
      assert(swidth < (2<<20) && sheight < (2<<20));
      const BBox2f srange(Vec2f(0.0f,0.0f),Vec2f(swidth-1,sheight-1));
      const BBox2f erange(Vec2f(x0,y0),Vec2f(x1,y1));

      /* convert into standard quad patch if possible */
      if (likely(patch.isQuadPatch())) 
      {
        CatmullClarkPatch3fa qpatch; patch.init(qpatch);
        eval(qpatch, srange, erange, 0);
        assert(count == (x1-x0+1)*(y1-y0+1));
        return;
      }

      /* subdivide patch */
      size_t N;
      array_t<CatmullClarkPatch3fa,GeneralCatmullClarkPatch3fa::SIZE> patches; 
      patch.subdivide(patches,N);

      if (N == 4)
      {
        const Vec2f c = srange.center();
        const BBox2f srange0(srange.lower,c);
        const BBox2f srange1(Vec2f(c.x,srange.lower.y),Vec2f(srange.upper.x,c.y));
        const BBox2f srange2(c,srange.upper);
        const BBox2f srange3(Vec2f(srange.lower.x,c.y),Vec2f(c.x,srange.upper.y));
        
        GeneralCatmullClarkPatch3fa::fix_quad_ring_order(patches);
        eval(patches[0],srange0,intersect(srange0,erange),1);
        eval(patches[1],srange1,intersect(srange1,erange),1);
        eval(patches[2],srange2,intersect(srange2,erange),1);
        eval(patches[3],srange3,intersect(srange3,erange),1);
      }
      else
      {
        assert(subPatch < N);
        eval(patches[subPatch], srange, erange, 1);
      }
      assert(count == (x1-x0+1)*(y1-y0+1));
    }

    template<typename Patch>
    __forceinline void evalLocalGrid(const Patch& patch, const BBox2f& srange, const int lx0, const int lx1, const int ly0, const int ly1)
    {
      const float scale_x = rcp(srange.upper.x-srange.lower.x);
      const float scale_y = rcp(srange.upper.y-srange.lower.y);
      count += (lx1-lx0)*(ly1-ly0);

#if 1
      for (size_t iy=ly0; iy<ly1; iy++) {
        for (size_t ix=lx0; ix<lx1; ix++) {
          const float lu = select(ix == swidth -1, float(1.0f), (float(ix)-srange.lower.x)*scale_x);
          const float lv = select(iy == sheight-1, float(1.0f), (float(iy)-srange.lower.y)*scale_y);
          const Vec3fa p = patch.eval(lu,lv);
          const float u = float(ix)*rcp_swidth;
          const float v = float(iy)*rcp_sheight;
          const int ofs = (iy-y0)*dwidth+(ix-x0);
          Px[ofs] = p.x;
          Py[ofs] = p.y;
          Pz[ofs] = p.z;
          U[ofs] = u;
          V[ofs] = v;
        }
      }
#else
      foreach2(lx0,lx1,ly0,ly1,[&](const vbool& valid, const vint& ix, const vint& iy) {
          const vfloat lu = select(ix == swidth -1, vfloat(1.0f), (vfloat(ix)-srange.lower.x)*scale_x);
          const vfloat lv = select(iy == sheight-1, vfloat(1.0f), (vfloat(iy)-srange.lower.y)*scale_y);
          const Vec3<vfloat> p = patch.eval(lu,lv);
          const vfloat u = vfloat(ix)*rcp_swidth;
          const vfloat v = vfloat(iy)*rcp_sheight;
          const vint ofs = (iy-y0)*dwidth+(ix-x0);
          if (likely(all(valid)) && all(iy==iy[0])) {
            const size_t ofs2 = ofs[0];
            vfloat::storeu(Px+ofs2,p.x);
            vfloat::storeu(Py+ofs2,p.y);
            vfloat::storeu(Pz+ofs2,p.z);
            vfloat::storeu(U+ofs2,u);
            vfloat::storeu(V+ofs2,v);
          } else {
            foreach_unique_index(valid,iy,[&](const vbool& valid, const int iy0, const int j) {
                const size_t ofs2 = ofs[j]-j;
                vfloat::storeu(valid,Px+ofs2,p.x);
                vfloat::storeu(valid,Py+ofs2,p.y);
                vfloat::storeu(valid,Pz+ofs2,p.z);
                vfloat::storeu(valid,U+ofs2,u);
                vfloat::storeu(valid,V+ofs2,v);
              });
          }
        });
#endif
    }

    void eval(const CatmullClarkPatch3fa& patch, const BBox2f& srange, const BBox2f& erange, const size_t depth)
    {
      if (erange.empty())
	return;

      int lx0 = ceilf(erange.lower.x);
      int lx1 = ceilf(erange.upper.x) + (erange.upper.x == x1 && (srange.lower.x < erange.upper.x || erange.upper.x == 0));
      int ly0 = ceilf(erange.lower.y);
      int ly1 = ceilf(erange.upper.y) + (erange.upper.y == y1 && (srange.lower.y < erange.upper.y || erange.upper.y == 0));
      if (lx0 >= lx1 || ly0 >= ly1) return;
      
      if (unlikely(patch.isRegular2())) {
        RegularPatch rpatch(patch);
        evalLocalGrid(rpatch,srange,lx0,lx1,ly0,ly1);
        return;
      }
#if PATCH_USE_GREGORY == 2
      else if (unlikely(depth>=PATCH_MAX_EVAL_DEPTH || patch.isGregory())) {
        GregoryPatch gpatch(patch);
        evalLocalGrid(gpatch,srange,lx0,lx1,ly0,ly1);
        return;
      }
#else
      //else if (unlikely(depth>=PATCH_MAX_EVAL_DEPTH))
      else if (unlikely(patch.isFinalResolution() || depth>=PATCH_MAX_EVAL_DEPTH))
      {
#if PATCH_USE_GREGORY == 1
        GregoryPatch gpatch(patch);
        evalLocalGrid(gpatch,srange,lx0,lx1,ly0,ly1);
#else
        BilinearPatch bpatch(patch);
        evalLocalGrid(bpatch,srange,lx0,lx1,ly0,ly1);
#endif
        return;
      }
#endif
      else
      {
        array_t<CatmullClarkPatch3fa,4> patches; 
        patch.subdivide(patches);
        
        const Vec2f c = srange.center();
        const BBox2f srange0(srange.lower,c);
        const BBox2f srange1(Vec2f(c.x,srange.lower.y),Vec2f(srange.upper.x,c.y));
        const BBox2f srange2(c,srange.upper);
        const BBox2f srange3(Vec2f(srange.lower.x,c.y),Vec2f(c.x,srange.upper.y));
        
        eval(patches[0],srange0,intersect(srange0,erange),depth+1);
        eval(patches[1],srange1,intersect(srange1,erange),depth+1);
        eval(patches[2],srange2,intersect(srange2,erange),depth+1);
        eval(patches[3],srange3,intersect(srange3,erange),depth+1);
      }
    }
  };

  __forceinline void feature_adaptive_eval2 (const SubdivMesh::HalfEdge* h, size_t subPatch, const BufferT<Vec3fa>& vertices,
                                             const size_t x0, const size_t x1, const size_t y0, const size_t y1, const size_t swidth, const size_t sheight, 
                                             float* Px, float* Py, float* Pz, float* U, float* V, const size_t dwidth, const size_t dheight)
  {
    GeneralCatmullClarkPatch3fa patch;
    patch.init(h,vertices);
    FeatureAdaptiveEval2(patch,subPatch,x0,x1,y0,y1,swidth,sheight,Px,Py,Pz,U,V,dwidth,dheight);
  }

  static __forceinline bool stitch_col(const GeneralCatmullClarkPatch3fa& patch, int subPatch,
                                       const bool right, const size_t y0, const size_t y1, const int fine_y, const int coarse_y, 
                                       float* Px, float* Py, float* Pz, float* U, float* V, const size_t dx0, const size_t dwidth, const size_t dheight)
  {
    assert(coarse_y <= fine_y);
    if (likely(fine_y == coarse_y))
      return false;
    
    const size_t y0s = stitch(y0,fine_y,coarse_y);
    const size_t y1s = stitch(y1,fine_y,coarse_y);
    assert(y1s-y0s < 4097);
    
    float px[4097], py[4097], pz[4097], u[4097], v[4097]; // FIXME: limits maximal level
    FeatureAdaptiveEval2(patch,subPatch, right,right, y0s,y1s, 2,coarse_y+1, px,py,pz,u,v, 1,4097);
    
    for (int y=y0; y<=y1; y++) {
      const size_t ys = stitch(y,fine_y,coarse_y)-y0s;
      Px[(y-y0)*dwidth+dx0] = px[ys];
      Py[(y-y0)*dwidth+dx0] = py[ys];
      Pz[(y-y0)*dwidth+dx0] = pz[ys];
      U [(y-y0)*dwidth+dx0] = u[ys];
      V [(y-y0)*dwidth+dx0] = v[ys];
    }
    return true;
  }
  
  static __forceinline bool stitch_row(const GeneralCatmullClarkPatch3fa& patch, int subPatch,
                                     const bool bottom, const size_t x0, const size_t x1, const int fine_x, const int coarse_x, 
                                     float* Px, float* Py, float* Pz, float* U, float* V, const size_t dy0, const size_t dwidth, const size_t dheight)
    {
      assert(coarse_x <= fine_x);
      if (likely(fine_x == coarse_x))
	return false;

      const size_t x0s = stitch(x0,fine_x,coarse_x);
      const size_t x1s = stitch(x1,fine_x,coarse_x);
      assert(x1s-x0s < 4097);
      
      float px[4097], py[4097], pz[4097], u[4097], v[4097]; // FIXME: limits maximal level
      FeatureAdaptiveEval2(patch,subPatch, x0s,x1s, bottom,bottom, coarse_x+1,2, px,py,pz,u,v, 4097,1);
      
      for (int x=x0; x<=x1; x++) {
	const size_t xs = stitch(x,fine_x,coarse_x)-x0s;
	Px[dy0*dwidth+x-x0] = px[xs];
        Py[dy0*dwidth+x-x0] = py[xs];
        Pz[dy0*dwidth+x-x0] = pz[xs];
        U [dy0*dwidth+x-x0] = u[xs];
        V [dy0*dwidth+x-x0] = v[xs];
      }
      return true;
    }
  
  __forceinline void feature_adaptive_eval2 (const SubdivMesh::HalfEdge* h, size_t subPatch, const float levels[4], const BufferT<Vec3fa>& vertices,
                                             const size_t x0, const size_t x1, const size_t y0, const size_t y1, const size_t swidth, const size_t sheight, 
                                             float* Px, float* Py, float* Pz, float* U, float* V, const size_t dwidth, const size_t dheight)
  {
    GeneralCatmullClarkPatch3fa patch;
    patch.init(h,vertices);
    
    const bool sl = x0 == 0         && stitch_col(patch,subPatch,0,y0,y1,sheight-1,levels[3], Px,Py,Pz,U,V, 0    ,dwidth,dheight);
    const bool sr = x1 == swidth-1  && stitch_col(patch,subPatch,1,y0,y1,sheight-1,levels[1], Px,Py,Pz,U,V, x1-x0,dwidth,dheight);
    
    const bool st = y0 == 0         && stitch_row(patch,subPatch,0,x0,x1,swidth-1,levels[0], Px,Py,Pz,U,V, 0    ,dwidth,dheight);
    const bool sb = y1 == sheight-1 && stitch_row(patch,subPatch,1,x0,x1,swidth-1,levels[2], Px,Py,Pz,U,V, y1-y0,dwidth,dheight);

    const size_t ofs = st*dwidth+sl;
    FeatureAdaptiveEval2(patch,subPatch,x0+sl,x1-sr,y0+st,y1-sb, swidth,sheight, Px+ofs,Py+ofs,Pz+ofs,U+ofs,V+ofs, dwidth,dheight);
  }


  }
}

