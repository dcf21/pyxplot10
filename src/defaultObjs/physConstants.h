// physConstants.h
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
// $Id$
//
// Pyxplot is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// You should have received a copy of the GNU General Public License along with
// Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
// Street, Fifth Floor, Boston, MA  02110-1301, USA

// ----------------------------------------------------------------------------

// The physical constants used by Pyxplot's phy module and unit database, in SI units. These replace the constants
// supplied by GSL, which date from CODATA 2006 or earlier. Measured values are the CODATA 2022 recommended values;
// constants which have exact values in the SI since 2019 are given exactly. Astronomical values are the IAU 2012
// definition of the astronomical unit and the IAU 2015 nominal solar and planetary values (Resolution B3).

#ifndef _PHYSCONSTANTS_H
#define _PHYSCONSTANTS_H 1

#include <math.h>

// Constants with exact values in the SI
#define PPL_CONST_C        299792458.0      // Speed of light in vacuum / m s^-1
#define PPL_CONST_H        6.62607015e-34   // Planck constant / J s
#define PPL_CONST_Q        1.602176634e-19  // Elementary charge / C
#define PPL_CONST_KB       1.380649e-23     // Boltzmann constant / J K^-1
#define PPL_CONST_NA       6.02214076e23    // Avogadro constant / mol^-1
#define PPL_CONST_GRAV_ACC 9.80665          // Standard acceleration of gravity / m s^-2

// Constants derived exactly from the above
#define PPL_CONST_HBAR  (PPL_CONST_H / (2 * M_PI))     // Reduced Planck constant / J s
#define PPL_CONST_R     (PPL_CONST_NA * PPL_CONST_KB)  // Molar gas constant / J mol^-1 K^-1
#define PPL_CONST_EV    PPL_CONST_Q                    // Electron volt / J

// Measured constants: CODATA 2022
#define PPL_CONST_MU_0      1.25663706127e-6  // Vacuum magnetic permeability / N A^-2
#define PPL_CONST_EPSILON_0 8.8541878188e-12  // Vacuum electric permittivity / F m^-1
#define PPL_CONST_G         6.67430e-11       // Newtonian constant of gravitation / m^3 kg^-1 s^-2
#define PPL_CONST_ALPHA     7.2973525643e-3   // Fine-structure constant
#define PPL_CONST_M_E       9.1093837139e-31  // Electron mass / kg
#define PPL_CONST_M_P       1.67262192595e-27 // Proton mass / kg
#define PPL_CONST_M_N       1.67492750056e-27 // Neutron mass / kg
#define PPL_CONST_M_MUON    1.883531627e-28   // Muon mass / kg
#define PPL_CONST_M_U       1.66053906892e-27 // Atomic mass constant / kg
#define PPL_CONST_MU_B      9.2740100657e-24  // Bohr magneton / J T^-1
#define PPL_CONST_RYDBERG   10973731.568157   // Rydberg constant / m^-1

// Astronomical constants
#define PPL_CONST_AU         149597870700.0                       // Astronomical unit / m (IAU 2012)
#define PPL_CONST_PARSEC     (PPL_CONST_AU * 648000 / M_PI)       // Parsec / m (IAU 2015)
#define PPL_CONST_LIGHT_YEAR (PPL_CONST_C * 365.25 * 86400)       // Light year / m (Julian year)
#define PPL_CONST_GM_SUN     1.3271244e20                         // Nominal solar mass parameter / m^3 s^-2
#define PPL_CONST_R_SUN      6.957e8                              // Nominal solar radius / m
#define PPL_CONST_L_SUN      3.828e26                             // Nominal solar luminosity / W
#define PPL_CONST_GM_EARTH   3.986004e14                          // Nominal Earth mass parameter / m^3 s^-2
#define PPL_CONST_GM_JUPITER 1.2668653e17                         // Nominal Jupiter mass parameter / m^3 s^-2
#define PPL_CONST_M_SUN      (PPL_CONST_GM_SUN / PPL_CONST_G)     // Solar mass / kg
#define PPL_CONST_M_EARTH    (PPL_CONST_GM_EARTH / PPL_CONST_G)   // Earth mass / kg
#define PPL_CONST_M_JUPITER  (PPL_CONST_GM_JUPITER / PPL_CONST_G) // Jupiter mass / kg

// Constants derived from those above, which require calls to sqrt() or pow()

//! ppl_constSigma - The Stefan-Boltzmann constant, derived exactly from h, c and k_B
//! \return The Stefan-Boltzmann constant / W m^-2 K^-4
static inline double ppl_constSigma(void) {
    return 2 * pow(M_PI, 5) * pow(PPL_CONST_KB, 4) / (15 * pow(PPL_CONST_H, 3) * pow(PPL_CONST_C, 2));
}

//! ppl_constPlanckLength - The Planck length, sqrt(hbar G / c^3)
//! \return The Planck length / m
static inline double ppl_constPlanckLength(void) { return sqrt(PPL_CONST_HBAR * PPL_CONST_G / pow(PPL_CONST_C, 3)); }

//! ppl_constPlanckMass - The Planck mass, sqrt(hbar c / G)
//! \return The Planck mass / kg
static inline double ppl_constPlanckMass(void) { return sqrt(PPL_CONST_HBAR * PPL_CONST_C / PPL_CONST_G); }

//! ppl_constPlanckTime - The Planck time, the Planck length divided by c
//! \return The Planck time / s
static inline double ppl_constPlanckTime(void) { return ppl_constPlanckLength() / PPL_CONST_C; }

//! ppl_constPlanckCharge - The Planck charge, sqrt(4 pi epsilon_0 hbar c)
//! \return The Planck charge / C
static inline double ppl_constPlanckCharge(void) {
    return sqrt(4 * M_PI * PPL_CONST_EPSILON_0 * PPL_CONST_HBAR * PPL_CONST_C);
}

//! ppl_constPlanckTemperature - The Planck temperature, m_P c^2 / k_B
//! \return The Planck temperature / K
static inline double ppl_constPlanckTemperature(void) {
    return ppl_constPlanckMass() * PPL_CONST_C * PPL_CONST_C / PPL_CONST_KB;
}

#endif
