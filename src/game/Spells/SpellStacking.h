/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _SPELLSTACKING_H
#define _SPELLSTACKING_H

#include "Server/DBCEnums.h"

struct SpellEntry;
class Unit;

enum class SpellGroupRule
{
    UNIQUE_PER_TARGET,
    UNIQUE_PER_CASTER,
};

class SpellStacker
{
    public:
        SpellStacker() {}
        ~SpellStacker() {}

        static SpellStacker& Instance();

        bool IsStackableAuraEffect(SpellEntry const* entry, SpellEntry const* entry2, SpellEffectIndex effIdx, Unit* target) const;
        bool IsStackableSpell(SpellEntry const* entry, SpellEntry const* entry2, Unit* target) const;
        bool IsSpellStackableWithSpell(SpellEntry const* entry1, SpellEntry const* entry2, Unit* target) const;
        bool IsSpellStackableWithSpellForDifferentCasters(SpellEntry const* entry1, SpellEntry const* entry2, bool isSameChain, Unit* target) const;
};

#define sSpellStacker SpellStacker::Instance()

#endif