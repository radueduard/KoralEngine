//
// kphysics: rigid bodies on the CPU. Spheres, boxes, capsules and planes; contacts by sequential impulses with
// friction, restitution, warm starting and split impulse; ball, hinge and distance joints; islands and sleep; four
// broadphases. Ported from the Kotlin core of the physics project (EngineProjects/physics), which it is checked
// against: the same tests, and the same reference runs within a tolerance.
//
// It depends on nothing, as kecs depends on nothing. The engine reaches it through the PhysicsSimulation, RigidBody,
// Collider and Joint scripts (keditor), which step a Simulation on the fixed step.
//

#pragma once

#include <kphysics/bodies.h>
#include <kphysics/broadphase.h>
#include <kphysics/collision.h>
#include <kphysics/islands.h>
#include <kphysics/joints.h>
#include <kphysics/math.h>
#include <kphysics/recording.h>
#include <kphysics/simulation.h>
#include <kphysics/solver.h>
#include <kphysics/thread_pool.h>
