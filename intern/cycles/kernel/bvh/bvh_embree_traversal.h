/*
 * Copyright 2017 , Blender Foundation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "embree2/rtcore_ray.h"
#include "embree2/rtcore_scene.h"

struct RTCORE_ALIGN(16) CCLRay : public RTCRay {
	typedef enum {
		RAY_REGULAR = 0,
		RAY_SHADOW_ALL = 1,
		RAY_SSS = 2,
		RAY_VOLUME_ALL = 3,
		
	} RayType;

	// cycles extensions:
	ccl::KernelGlobals *kg;
	RayType type;
	ccl::uint shadow_linking;
	/* object and primitive of the ray start */
	int object;
	int prim;

	/* object, primitive and distance of the last successful intersection test */
	int object_test;
	int prim_test;
	float t_test;

	// for shadow rays
	ccl::Intersection *isect_s;
	int max_hits;
	int num_hits;

	// for SSS Rays:
	ccl::SubsurfaceIntersection *ss_isect;
	int sss_object_id;
	ccl::uint *lcg_state;

	CCLRay(const ccl::Ray& ray, ccl::KernelGlobals *kg_, const ccl::uint visibility, RayType type_, const ccl::uint shadow_linking_)
	{
		org[0] = ray.P.x;
		org[1] = ray.P.y;
		org[2] = ray.P.z;
		dir[0] = ray.D.x;
		dir[1] = ray.D.y;
		dir[2] = ray.D.z;
		tnear = ray.t_near;
		tfar = ray.t;
		time = ray.time;
		mask = visibility;
		geomID = primID = instID = RTC_INVALID_GEOMETRY_ID;

		prim = ray.prim;
		object = ray.object;
		object_test = OBJECT_NONE;
		prim_test = PRIM_NONE;
		t_test = -1.0f;

		kg = kg_;
		type = type_;
		shadow_linking = shadow_linking_;
		max_hits = 1;
		num_hits = 0;
		isect_s = NULL;
		ss_isect = NULL;
		sss_object_id = OBJECT_NONE;
		lcg_state = NULL;
	}

	void isect_to_ccl(ccl::Intersection *isect)
	{
		bool is_hair = geomID & 1;
		isect->u = is_hair ? u : 1.0f - v - u;
		isect->v = is_hair ? v : u;
		isect->t = tfar;
		isect->Ng = ccl::make_float3(Ng[0], Ng[1], Ng[2]);
		if(instID != RTC_INVALID_GEOMETRY_ID) {
			RTCScene inst_scene = (RTCScene)rtcGetUserData(kernel_data.bvh.scene, instID);
			isect->prim = primID + (intptr_t)rtcGetUserData(inst_scene, geomID) + kernel_tex_fetch(__object_node, instID/2);
			isect->object = instID/2;
		} else {
			isect->prim = primID + (intptr_t)rtcGetUserData(kernel_data.bvh.scene, geomID);
			isect->object = OBJECT_NONE;
		}
		isect->type = kernel_tex_fetch(__prim_type, isect->prim);
	}
};
