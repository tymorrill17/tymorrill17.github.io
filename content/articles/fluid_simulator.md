+++
title = "Interactive Real-Time Fluid Simulator"
template = "article"
date = "April 13, 2026"
description = "Real-time, particle-based fluid simulation implementation."
draft = true
+++

# {{title}}

This is an overview of my implementation of a particle-based fluid simulation using the smoothed-particle hydrodymanics (SPH) method.
This version is implemented using multiple cores on the CPU via multithreading. The visualization is done using my own [renderering engine](/articles/renderer.html)
The program is written in [Odin](https://odin-lang.org/).^[Odin is a C-like systems programming language. It aims to keep the control and simplicity of C while
providing modern conveniences. It's a blast to use for graphics and game programming.]

I decided to go with the SPH method because it is fast, relatively stable, and straightforward to parallelize.

# Smoothed-Particle Hydrodynamics

Smoothed-Particle Hydrodynamics (SPH) was initially developed for astrophysics. It provides a way to interpolate field quantities of a fluid
using values carried by discrete particles. Each particle has a position, velocity, and acceleration, which are determined by the fluid itself,
along with quantities relevant to the fluid equation like density or viscosity. The total field value may be found at any
point in space by interpolating each particle's contribution using a smoothing kernel. In mathematical notation, for a query point $r$, a scalar field
quantity $A$ can be represented as

$$A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}W(r-r_j, h), \tag{1}$$

where $j$ loops over each particle and $m_j, A_j, \rho_j, r_j$ are the mass, field quantity, density, and position of particle $j$, respectively.
$W(r,h)$ is the smoothing kernel^[There are many options for the smoothing kernel, and
you need not use the same one for each quantity being found. For example, you may want a kernel with a higher rate of change near
the center for finding pressure and a more smooth kernel like a gaussian for density. ![A cubic spline kernel and its gradient](/images/kernels.svg)] with radius $h$ which satisfies

$$\int_{\Omega} W(r,h)dr = 1$$

for any $h$.^[Here, $\Omega$ is the entire domain of the simulation.]

Intuitively, we are looping over each particle and adding its weighted contribution to the total quantity's value. In practice, $h$ acts as
a cutoff value since the contribution becomes negligible the larger the distance becomes.^[Spoiler alert! I wonder if we can save computation by
avoiding looping over these zero-contribution particles...]

For our case, let's just assume mass is constant between particles, $m_j = 1$ so we can forget about it, it won't make a difference. That leaves us
with finding $\rho_j$, which we can do using our SPH equation $(1)$ above.

$$\rho_S(r) = \sum_j m_j \frac{\rho_j}{\rho_j}W(r-r_j,h) = \sum_j m_j W(r-r_j,h)$$

This is the main mechanism of the method. There is one more crucial property of $(1)$: we can use it to find the gradient or Laplacian of our
desired quantities as well.

$$ \nabla A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}\nabla W(r-r_j, h) \tag{2} $$

$$ \nabla^2 A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}\nabla^2 W(r-r_j, h) \tag{3} $$

Now let's take a look at the equation to be solved.

# The Navier-Stokes Equation

We will concern ourselves with the following simplified Navier-Stokes equation:

$$ \rho\frac{dv}{dt} = -\nabla P + \rho g + \mu \nabla^2 v + \textbf{f}_e \tag{4} $$

Here, $-\nabla P$ is the pressure force, $\rho g$ is the gravitational force, $\mu \nabla^2 v$ is the viscosity term,^[This implementation ignores
the viscosity term $\mu \nabla^2 v$ for now, since it can cause the problem to become ill-posed. It will be tackled in a later post.] and $\bf{f}_e$ are the
external forces. If you look closely, and note that $\rho$ is a mass analogue, you can see Newton's Second Law.

$$ \begin{aligned}
         F_{total} &= -\nabla P + \rho g + \mu \nabla^2 v + \textbf{f}_e \\
                 a &= \frac{dv}{dt} \\
\implies F_{total} &= \rho a
\end{aligned} $$

## Pressure Force

Just like with density, use equation $(1)$ to find the pressure force.

$$ -\nabla P(r) = -\sum_jm_j\frac{P_j}{\rho_j}\nabla W(r-r_j, h) \tag{5} $$

There are a couple of things to consider here. First, $P_j$ may be obtained from the ideal gas equation
$$P_j = \rho_j k,$$
where $k$ is a constant. It is common to use a modification of the ideal gas equation
$$P_j = k(\rho_j - \rho_0),$$
which has no effect on the pressure force since it is a gradient, but is more numerically stable.

Second, equation $(5)$ does not accurately compute the pressure force since it is not symmetric. Consider the interaction
between two particles, $r_i$ and $r_j$. Using $(5)$ and the properties of the smoothing kernel $W$,^[$\nabla W(0, h) = 0$
and $\nabla W(-r,h) = -\nabla W(r,h)$] Newton's Third Law says

$$ \begin{aligned}
& -\nabla P(r_i) = \nabla P(r_j) \\
\implies & -m_j \frac{P_j}{\rho_j} \nabla W(r_i - r_j) = -m_i \frac{P_i}{\rho_i} \nabla W(r_i - r_j) \\
\implies & \frac{P_j}{\rho_j} = \frac{P_i}{\rho_i} \\
\implies & \frac{k(p_j - p_0)}{\rho_j} = \frac{k(p_j - p_0)}{\rho_i} \\
\implies & \rho_j = \rho_i
\end{aligned} $$

which are not guaranteed to be equal. In order to remedy this, use the following substitution for equation $(5)$ that uses the mean
between the two pressure values to force symmetry.

$$-\nabla P(r_i) = -\sum_jm_j\frac{P_i + P_j}{2\rho_j}\nabla W(r-r_j, h), \tag{6}$$

# Spatial Partitioning

Currently, there is a serious unaddressed performance issue present in the algorithm. Every time equation $(1)$ is used, there is an
$\mathcal{O}(N^2)$ loop, where $N$ is the number of particles. Using our smoothing kernel, there is a way to

Currently, this algorithm has an $\mathcal{O}(N^2)$ each time equation $(1)$ is used, which is quite far from ideal. However, as hinted
previously, there is a sensical way to reduce this complexity. By partitioning the domain into a grid


