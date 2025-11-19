/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company. 

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).  

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// cg_marks.c -- wall marks

#include "cg_local.h"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#include <threads.h>
#include <stdatomic.h>
#define CG_MARKS_MULTITHREADED
#endif


/*
===================================================================

MARK POLYS

===================================================================
*/


markPoly_t cg_activeMarkPolys;          // double linked list
markPoly_t  *cg_freeMarkPolys;          // single linked list
markPoly_t cg_markPolys[MAX_MARK_POLYS];

/*
===================
CG_InitMarkPolys

This is called at startup and for tournement restarts
===================
*/
void    CG_InitMarkPolys( void ) {
	int i;
	markPoly_t *trav, *lasttrav;

	memset( cg_markPolys, 0, sizeof( cg_markPolys ) );

	cg_activeMarkPolys.nextMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.prevMark = &cg_activeMarkPolys;
	cg_freeMarkPolys = cg_markPolys;
	for ( i = 0, trav = cg_markPolys + 1, lasttrav = cg_markPolys ; i < MAX_MARK_POLYS - 1 ; i++, trav++ ) {
		lasttrav->nextMark = trav;
		lasttrav = trav;
	}
}


/*
==================
CG_FreeMarkPoly
==================
*/
void CG_FreeMarkPoly( markPoly_t *le ) {
	if ( !le->prevMark || !le->nextMark ) {
		CG_Error( "CG_FreeLocalEntity: not active" );
	}

	// remove from the doubly linked active list
	le->prevMark->nextMark = le->nextMark;
	le->nextMark->prevMark = le->prevMark;

	// the free list is only singly linked
	le->nextMark = cg_freeMarkPolys;
	cg_freeMarkPolys = le;
}

/*
===================
CG_AllocMark

Will allways succeed, even if it requires freeing an old active mark
===================
*/
markPoly_t  *CG_AllocMark( int endTime ) {
	markPoly_t  *le; //, *trav, *lastTrav;
	int time;

	if ( !cg_freeMarkPolys ) {
		// no free entities, so free the one at the end of the chain
		// remove the oldest active entity
		time = cg_activeMarkPolys.prevMark->time;
		while ( cg_activeMarkPolys.prevMark && time == cg_activeMarkPolys.prevMark->time ) {
			CG_FreeMarkPoly( cg_activeMarkPolys.prevMark );
		}
	}

	le = cg_freeMarkPolys;
	cg_freeMarkPolys = cg_freeMarkPolys->nextMark;

	memset( le, 0, sizeof( *le ) );

	// Ridah, TODO: sort this, so the list is always sorted by longest duration -> shortest duration,
	// this way the shortest duration mark will always get overwritten first
	//for (trav = cg_activeMarkPolys.nextMark; (trav->duration + trav->time > endTime) && (trav != cg_activeMarkPolys.prevMark) ; lastTrav = trav, trav++ ) {
	// Respect the FOR loop
	//}

	// link into the active list
	le->nextMark = cg_activeMarkPolys.nextMark;
	le->prevMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.nextMark->prevMark = le;
	cg_activeMarkPolys.nextMark = le;
	return le;
}



/*
=================
CG_ImpactMark

origin should be a point within a unit of the plane
dir should be the plane normal

temporary marks will not be stored or randomly oriented, but immediately
passed to the renderer.
=================
*/
// Ridah, increased this since we leave them around for longer
#define MAX_MARK_FRAGMENTS  1024 // RealRTCW was 384
#define MAX_MARK_POINTS     2048 // RealRTCW was 1024
//#define	MAX_MARK_FRAGMENTS	128
//#define	MAX_MARK_POINTS		384

// these are ignored now for the most part
//#define	MARK_TOTAL_TIME		20000	// (SA) made this a cvar: cg_markTime  (we could cap the time or remove marks quicker if too long a time starts to cause new marks to not appear)
#define MARK_FADE_TIME      30000

void CG_ImpactMark( qhandle_t markShader, const vec3_t origin, const vec3_t dir,
					float orientation, float red, float green, float blue, float alpha,
					qboolean alphaFade, float radius, qboolean temporary, int duration ) {
	vec3_t axis[3];
	float texCoordScale;
	vec3_t originalPoints[4];
	byte colors[4];
	int i, j;
	int numFragments;
	markFragment_t markFragments[MAX_MARK_FRAGMENTS], *mf;
	vec5_t markPoints[MAX_MARK_POINTS];             // Ridah, made it vec5_t so it includes S/T
	vec3_t projection;
	int multMaxFragments = 1;

	if ( !cg_markTime.integer ) {
		return;
	}

	if ( radius <= 0 ) {
		// just ignore it, don't error out
		return;
//		CG_Error( "CG_ImpactMark called with <= 0 radius" );
	}

	// Ridah, if no duration, use the default
	if ( duration < 0 ) {
		if ( duration == -2 ) {
			multMaxFragments = -1;  // use original mapping
		}

//		duration = MARK_TOTAL_TIME;
		duration = cg_markTime.integer;
	}

	// create the texture axis
	VectorNormalize2( dir, axis[0] );
	PerpendicularVector( axis[1], axis[0] );
	RotatePointAroundVector( axis[2], axis[0], axis[1], orientation );
	CrossProduct( axis[0], axis[2], axis[1] );

	texCoordScale = 0.5 * 1.0 / radius;

	// create the full polygon
	for ( i = 0 ; i < 3 ; i++ ) {
		originalPoints[0][i] = origin[i] - radius * axis[1][i] - radius * axis[2][i];
		originalPoints[1][i] = origin[i] + radius * axis[1][i] - radius * axis[2][i];
		originalPoints[2][i] = origin[i] + radius * axis[1][i] + radius * axis[2][i];
		originalPoints[3][i] = origin[i] - radius * axis[1][i] + radius * axis[2][i];
	}

	// get the fragments
	//VectorScale( dir, -20, projection );
	VectorScale( dir, radius * 2, projection );
	numFragments = trap_CM_MarkFragments( (int)orientation, (void *)originalPoints,
										  projection, MAX_MARK_POINTS, (float *)&markPoints[0],
										  MAX_MARK_FRAGMENTS * multMaxFragments, markFragments );

	colors[0] = red * 255;
	colors[1] = green * 255;
	colors[2] = blue * 255;
	colors[3] = alpha * 255;

	for ( i = 0, mf = markFragments ; i < numFragments ; i++, mf++ ) {
		polyVert_t  *v;
		polyVert_t verts[MAX_VERTS_ON_POLY];
		markPoly_t  *mark;
		qboolean hasST;

		// we have an upper limit on the complexity of polygons
		// that we store persistantly
		if ( mf->numPoints > MAX_VERTS_ON_POLY ) {
			mf->numPoints = MAX_VERTS_ON_POLY;
		}
		if ( mf->numPoints < 0 ) {
			hasST = qtrue;
			mf->numPoints *= -1;
		} else {
			hasST = qfalse;
		}
		for ( j = 0, v = verts ; j < mf->numPoints ; j++, v++ ) {
			vec3_t delta;

			VectorCopy( markPoints[mf->firstPoint + j], v->xyz );

			if ( !hasST ) {
				VectorSubtract( v->xyz, origin, delta );
				v->st[0] = 0.5 + DotProduct( delta, axis[1] ) * texCoordScale;
				v->st[1] = 0.5 + DotProduct( delta, axis[2] ) * texCoordScale;
			} else {
				v->st[0] = markPoints[mf->firstPoint + j][3];
				v->st[1] = markPoints[mf->firstPoint + j][4];
			}

			*(int *)v->modulate = *(int *)colors;
		}

		// if it is a temporary (shadow) mark, add it immediately and forget about it
		if ( temporary ) {
			trap_R_AddPolyToScene( markShader, mf->numPoints, verts );
			continue;
		}

		// otherwise save it persistantly
		mark = CG_AllocMark( cg.time + duration );
		mark->time = cg.time;
		mark->alphaFade = alphaFade;
		mark->markShader = markShader;
		mark->poly.numVerts = mf->numPoints;
		mark->color[0] = red;
		mark->color[1] = green;
		mark->color[2] = blue;
		mark->color[3] = alpha;
		mark->duration = duration;
		memcpy( mark->verts, verts, mf->numPoints * sizeof( verts[0] ) );
	}
}


/*
===============
CG_AddMarks
===============
*/

#ifdef CG_MARKS_MULTITHREADED

// Максимальное количество потоков для обработки марок.
// R_GetHardwareThreadCount() обычно возвращает до 128.
#define MAX_MARK_THREADS 128

typedef struct {
	markPoly_t **start;
	int count;
	int time;
} mark_thread_arg_t;

static mark_thread_arg_t mark_thread_args[MAX_MARK_THREADS];
static thrd_t mark_threads[MAX_MARK_THREADS];

static void CG_ProcessMarkPoly(markPoly_t *mp, int time) {
	int j;
	int fade;
	int t;

	// fade out the energy bursts
	if (mp->markShader == cgs.media.energyMarkShader) {
		fade = 450 - 450 * ((time - mp->time) / 3000.0f);
		if (fade < 255) {
			if (fade < 0) fade = 0;
			for (j = 0; j < mp->poly.numVerts; j++) {
				mp->verts[j].modulate[0] = mp->color[0] * fade;
				mp->verts[j].modulate[1] = mp->color[1] * fade;
				mp->verts[j].modulate[2] = mp->color[2] * fade;
			}
		}
	}

	// fade in the zombie spirit marks
	if (mp->markShader == cgs.media.zombieSpiritWallShader) {
		fade = 255 * ((time - mp->time) / 2000.0f);
		if (fade < 255) {
			if (fade < 0) fade = 0;
			for (j = 0; j < mp->poly.numVerts; j++) {
				mp->verts[j].modulate[0] = mp->color[0] * fade;
				mp->verts[j].modulate[1] = mp->color[1] * fade;
				mp->verts[j].modulate[2] = mp->color[2] * fade;
			}
		}
	}

	// fade all marks out with time
	t = mp->time + mp->duration - time;
	if (t < (float)mp->duration / 2.0f) {
		fade = (int)(255.0f * (float)t / ((float)mp->duration / 2.0f));
		if (mp->alphaFade) {
			for (j = 0; j < mp->poly.numVerts; j++) mp->verts[j].modulate[3] = fade;
		} else {
			for (j = 0; j < mp->poly.numVerts; j++) {
				mp->verts[j].modulate[0] = mp->color[0] * fade;
				mp->verts[j].modulate[1] = mp->color[1] * fade;
				mp->verts[j].modulate[2] = mp->color[2] * fade;
			}
		}
	}
}

static int CG_AddMarks_Thread(void *arg) {
	mark_thread_arg_t *thread_arg = (mark_thread_arg_t *)arg;
	int i;

	for (i = 0; i < thread_arg->count; i++) {
		CG_ProcessMarkPoly(thread_arg->start[i], thread_arg->time);
	}

	return 0;
}

#endif // CG_MARKS_MULTITHreadED

void CG_AddMarks( void ) {
	int j;
	markPoly_t  *mp, *next;
	int t;
	int fade;

	if ( !cg_markTime.integer ) {
		return;
	}

#ifdef CG_MARKS_MULTITHREADED
	{
		markPoly_t *polys_to_process[MAX_MARK_POLYS];
		markPoly_t *polys_to_free[MAX_MARK_POLYS];
		int process_count = 0;
		int free_count = 0;
		int num_threads, i, polys_per_thread, remainder;
		markPoly_t **current_poly_ptr;

		// Stage 1: Collect polygons for processing and removal in the main thread
		mp = cg_activeMarkPolys.nextMark;
		while (mp != &cg_activeMarkPolys) {
			if (cg.time > mp->time + mp->duration) {
				if (free_count < MAX_MARK_POLYS) {
					polys_to_free[free_count++] = mp;
				}
			} else {
				if (process_count < MAX_MARK_POLYS) {
					polys_to_process[process_count++] = mp;
				}
			}
			mp = mp->nextMark;
		}

		// Stage 2: Parallel processing of polygons
		if (process_count > 0) {
			num_threads = CG_GetHardwareThreadCount();
			if (num_threads > MAX_MARK_THREADS) num_threads = MAX_MARK_THREADS;
			if (num_threads > process_count) num_threads = process_count;
			if (num_threads < 1) num_threads = 1;

			polys_per_thread = process_count / num_threads;
			remainder = process_count % num_threads;
			current_poly_ptr = polys_to_process;

			for (i = 0; i < num_threads; i++) {
				mark_thread_args[i].start = current_poly_ptr;
				mark_thread_args[i].count = polys_per_thread + (i < remainder ? 1 : 0);
				mark_thread_args[i].time = cg.time;
				current_poly_ptr += mark_thread_args[i].count;
				thrd_create(&mark_threads[i], CG_AddMarks_Thread, &mark_thread_args[i]);
			}

			for (i = 0; i < num_threads; i++) {
				thrd_join(mark_threads[i], NULL);
			}
		}

		// Stage 3: Add to scene and free memory in the main thread
		for (i = 0; i < process_count; i++) {
			mp = polys_to_process[i];
			trap_R_AddPolyToScene(mp->markShader, mp->poly.numVerts, mp->verts);
		}

		for (i = 0; i < free_count; i++) {
			CG_FreeMarkPoly(polys_to_free[i]);
		}

		return;
	}
#endif

	// =================================================================
	// Original single-threaded code, which will be used
	// if the compiler does not support C11 threads.
	// =================================================================

	mp = cg_activeMarkPolys.nextMark;
	for ( ; mp != &cg_activeMarkPolys ; mp = next ) {
		// grab next now, so if the local entity is freed we
		// still have it
		next = mp->nextMark;

		// see if it is time to completely remove it
		if ( cg.time > mp->time + mp->duration ) {
			CG_FreeMarkPoly( mp );
			continue;
		}

		// fade out the energy bursts
		if ( mp->markShader == cgs.media.energyMarkShader ) {

			fade = 450 - 450 * ( ( cg.time - mp->time ) / 3000.0 );
			if ( fade < 255 ) {
				if ( fade < 0 ) {
					fade = 0;
				}
				if ( mp->verts[0].modulate[0] != 0 ) {
					for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
						mp->verts[j].modulate[0] = mp->color[0] * fade;
						mp->verts[j].modulate[1] = mp->color[1] * fade;
						mp->verts[j].modulate[2] = mp->color[2] * fade;
					}
				}
			}
		}

		// fade in the zombie spirit marks
		if ( mp->markShader == cgs.media.zombieSpiritWallShader ) {

			fade = 255 * ( ( cg.time - mp->time ) / 2000.0 );
			if ( fade < 255 ) {
				if ( fade < 0 ) {
					fade = 0;
				}
				if ( mp->verts[0].modulate[0] != 0 ) {
					for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
						mp->verts[j].modulate[0] = mp->color[0] * fade;
						mp->verts[j].modulate[1] = mp->color[1] * fade;
						mp->verts[j].modulate[2] = mp->color[2] * fade;
					}
				}
			}
		}

		// fade all marks out with time
		t = mp->time + mp->duration - cg.time;
		if ( t < (float)mp->duration / 2.0 ) {
			fade = (int)( 255.0 * (float)t / ( (float)mp->duration / 2.0 ) );
			if ( mp->alphaFade ) {
				for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
					mp->verts[j].modulate[3] = fade;
				}
			} else {
				for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
					mp->verts[j].modulate[0] = mp->color[0] * fade;
					mp->verts[j].modulate[1] = mp->color[1] * fade;
					mp->verts[j].modulate[2] = mp->color[2] * fade;
				}
			}
		}

		trap_R_AddPolyToScene( mp->markShader, mp->poly.numVerts, mp->verts );
	}
}
