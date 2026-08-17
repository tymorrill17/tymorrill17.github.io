+++
title = "Interactive Real-Time Fluid Simulator"
template = "article"
date = "April 13, 2026"
description = "Real-time, particle-based fluid simulation implementation."
draft = true
+++

# {{title}}

This is an overview of my implementation of a particle-based fluid simulation using the smoothed-particle hydrodymanics (SPH) method.
This version is implemented using multiple cores on the CPU via threading. The visualization is done using my own [renderer](/articles/renderer.html)
The program is written in [Odin](https://odin-lang.org/).^[Odin is a C-like systems programming language. It's a blast to use for graphics and
game programming.]

I decided to go with SPH initially because it is fast, relatively stable, and straightforward to parallelize.

# Smoothed-Particle Hydrodynamics

Smoothed-Particle Hydrodynamics (SPH) was initially developed for astrophysics. It is a method of discretizing field quantities using particles with
positions and velocities governed by the fluid. The total field value may be found at any point in space by interpolating each particle's
contribution using a smoothing kernel. More precisely, for a query point $r$, a scalar field quantity A can be represented as

$$A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}W(r-r_j, h),$$

where $j$ loops over each particle and $m_j, A_j, \rho_j, r_j$ are the mass, field quantity, density, and position of particle $j$, respectively. $W(r,h)$ is the
smoothing kernel, which should have finite support defined by radius $h$, and should satisfy

$$\int W(r,h)dr = 1$$

for any $h$.



