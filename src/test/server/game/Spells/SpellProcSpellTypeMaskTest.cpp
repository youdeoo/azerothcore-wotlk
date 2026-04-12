/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AuraScriptTestFramework.h"
#include "SpellMgr.h"
#include "gtest/gtest.h"

/**
 * @brief Tests for SpellTypeMask calculation based on proc phase
 *
 * These tests verify that the proc system correctly calculates SpellTypeMask
 * for different proc phases. This is critical because:
 * - CAST phase: No damage/heal has occurred yet
 * - HIT phase: Damage/heal info is available
 * - FINISH phase: damageInfo may be null even for damage spells
 *
 * Regression tests for:
 * - FINISH phase incorrectly classifying damage-over-time casts as non-damage
 * - FINISH phase being too broad and letting utility spells satisfy
 *   DAMAGE/HEAL proc filters
 */
class SpellProcSpellTypeMaskTest : public AuraScriptProcTestFixture
{
protected:
    void SetUp() override
    {
        AuraScriptProcTestFixture::SetUp();
    }

    /**
     * @brief Calculate spellTypeMask the same way ProcSkillsAndAuras does
     *
     * This mirrors the logic in Unit::ProcSkillsAndAuras to allow unit testing
     * of the spellTypeMask calculation without needing full Unit objects.
     */
    static uint32 CalculateSpellTypeMask(uint32 procPhase, DamageInfo* damageInfo, HealInfo* healInfo, SpellInfo const* spellInfo)
    {
        uint32 spellTypeMask = 0;
        if (procPhase == PROC_SPELL_PHASE_CAST || procPhase == PROC_SPELL_PHASE_FINISH)
        {
            bool reliable = false;

            if (spellInfo)
                spellTypeMask = spellInfo->GetStaticProcSpellTypeMask(reliable);

            if (!reliable)
                spellTypeMask = PROC_SPELL_TYPE_MASK_ALL;
        }
        else if (healInfo && healInfo->GetHeal())
            spellTypeMask = PROC_SPELL_TYPE_HEAL;
        else if (damageInfo && damageInfo->GetDamage())
            spellTypeMask = PROC_SPELL_TYPE_DAMAGE;
        else if (spellInfo)
        {
            bool reliable = false;
            uint32 staticSpellTypeMask = spellInfo->GetStaticProcSpellTypeMask(reliable);

            if (reliable && (staticSpellTypeMask & (PROC_SPELL_TYPE_DAMAGE | PROC_SPELL_TYPE_HEAL)))
                spellTypeMask = staticSpellTypeMask & (PROC_SPELL_TYPE_DAMAGE | PROC_SPELL_TYPE_HEAL);
            else
                spellTypeMask = PROC_SPELL_TYPE_NO_DMG_HEAL;
        }

        return spellTypeMask;
    }
};

// =============================================================================
// SpellTypeMask Calculation Tests
// =============================================================================

TEST_F(SpellProcSpellTypeMaskTest, CastPhase_UsesStaticDamageTypeWhenReliable)
{
    auto* spellInfo = SpellInfoBuilder()
        .WithId(700010)
        .WithEffect(EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE)
        .Build();
    _spellInfos.push_back(spellInfo);

    uint32 result = CalculateSpellTypeMask(PROC_SPELL_PHASE_CAST, nullptr, nullptr, spellInfo);
    EXPECT_EQ(result, PROC_SPELL_TYPE_DAMAGE);
}

TEST_F(SpellProcSpellTypeMaskTest, FinishPhase_UsesMaskAll_ForAmbiguousTriggeredSpell)
{
    auto* spellInfo = SpellInfoBuilder()
        .WithId(700011)
        .WithEffect(EFFECT_0, SPELL_EFFECT_DUMMY)
        .Build();
    _spellInfos.push_back(spellInfo);

    uint32 result = CalculateSpellTypeMask(PROC_SPELL_PHASE_FINISH, nullptr, nullptr, spellInfo);
    EXPECT_EQ(result, PROC_SPELL_TYPE_MASK_ALL);
}

TEST_F(SpellProcSpellTypeMaskTest, HitPhase_WithDamage_UsesDamageType)
{
    auto* spellInfo = CreateSpellInfo(12345, 15, 0);
    DamageInfo damageInfo(nullptr, nullptr, 100, spellInfo, SPELL_SCHOOL_MASK_FROST, SPELL_DIRECT_DAMAGE);

    uint32 result = CalculateSpellTypeMask(PROC_SPELL_PHASE_HIT, &damageInfo, nullptr, spellInfo);
    EXPECT_EQ(result, PROC_SPELL_TYPE_DAMAGE);
}

TEST_F(SpellProcSpellTypeMaskTest, HitPhase_WithHeal_UsesHealType)
{
    auto* spellInfo = CreateSpellInfo(12345, 15, 0);
    HealInfo healInfo(nullptr, nullptr, 100, spellInfo, SPELL_SCHOOL_MASK_HOLY);

    uint32 result = CalculateSpellTypeMask(PROC_SPELL_PHASE_HIT, nullptr, &healInfo, spellInfo);
    EXPECT_EQ(result, PROC_SPELL_TYPE_HEAL);
}

TEST_F(SpellProcSpellTypeMaskTest, HitPhase_NoDamageNoHeal_UsesNoDmgHeal)
{
    // HIT phase with no damage/heal info should use NO_DMG_HEAL
    auto* spellInfo = SpellInfoBuilder()
        .WithId(700012)
        .WithEffect(EFFECT_0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_STAT)
        .Build();
    _spellInfos.push_back(spellInfo);

    uint32 result = CalculateSpellTypeMask(PROC_SPELL_PHASE_HIT, nullptr, nullptr, spellInfo);
    EXPECT_EQ(result, PROC_SPELL_TYPE_NO_DMG_HEAL);
}

// =============================================================================
// Killing Machine Regression Test
// =============================================================================

/**
 * @brief Regression test for Killing Machine (51124) proc consumption
 *
 * Killing Machine has:
 * - SpellTypeMask = 1 (PROC_SPELL_TYPE_DAMAGE)
 * - SpellPhaseMask = 4 (PROC_SPELL_PHASE_FINISH)
 *
 * When Icy Touch is cast, the FINISH phase event must have a spellTypeMask
 * that includes DAMAGE for the proc to fire and consume the buff.
 *
 * The bug was: FINISH phase calculated spellTypeMask as NO_DMG_HEAL (4)
 * because damageInfo was null, causing the proc check to fail.
 */
TEST_F(SpellProcSpellTypeMaskTest, KillingMachine_FinishPhase_MatchesDamageTypeMask)
{
    // Killing Machine spell_proc entry
    auto procEntry = SpellProcEntryBuilder()
        .WithSpellFamilyName(15) // SPELLFAMILY_DEATHKNIGHT
        .WithSpellFamilyMask(flag96(2, 6, 0)) // Icy Touch, Frost Strike, Howling Blast
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS | PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellTypeMask(PROC_SPELL_TYPE_DAMAGE)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_FINISH)
        .WithAttributesMask(PROC_ATTR_REQ_SPELLMOD)
        .WithCharges(1)
        .Build();

    // Calculate what spellTypeMask FINISH phase would produce
    // (simulating Spell.cpp calling ProcSkillsAndAuras with nullptr damageInfo)
    auto* icyTouchSpell = CreateSpellInfo(49909, 15, 2);
    uint32 finishPhaseSpellTypeMask = CalculateSpellTypeMask(PROC_SPELL_PHASE_FINISH, nullptr, nullptr, icyTouchSpell);

    // Verify the calculated mask includes DAMAGE type
    EXPECT_TRUE(finishPhaseSpellTypeMask & PROC_SPELL_TYPE_DAMAGE)
        << "FINISH phase spellTypeMask must include PROC_SPELL_TYPE_DAMAGE for Killing Machine to work";

    // Verify that the proc entry's SpellTypeMask requirement is satisfied
    EXPECT_TRUE(finishPhaseSpellTypeMask & procEntry.SpellTypeMask)
        << "FINISH phase spellTypeMask (" << finishPhaseSpellTypeMask
        << ") must match Killing Machine's SpellTypeMask requirement (" << procEntry.SpellTypeMask << ")";
}

/**
 * @brief Verify FINISH phase works with actual CanSpellTriggerProcOnEvent
 *
 * This test verifies the full integration: when we pass the correctly
 * calculated spellTypeMask to CanSpellTriggerProcOnEvent, Killing Machine
 * style procs should work.
 */
TEST_F(SpellProcSpellTypeMaskTest, KillingMachine_FullIntegration_ProcTriggers)
{
    // Killing Machine spell_proc entry
    auto procEntry = SpellProcEntryBuilder()
        .WithSpellFamilyName(15) // SPELLFAMILY_DEATHKNIGHT
        .WithSpellFamilyMask(flag96(2, 0, 0)) // Icy Touch
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellTypeMask(PROC_SPELL_TYPE_DAMAGE)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_FINISH)
        .Build();

    // Create Icy Touch spell info (SpellFamilyFlags = [2, 0, 0])
    auto* icyTouchSpell = CreateSpellInfo(49909, 15, 2); // DK family, mask0=2
    DamageInfo damageInfo(nullptr, nullptr, 100, icyTouchSpell, SPELL_SCHOOL_MASK_FROST, SPELL_DIRECT_DAMAGE);

    // Create event with FINISH phase and DAMAGE type (as static classification provides)
    auto eventInfo = ProcEventInfoBuilder()
        .WithTypeMask(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithHitMask(PROC_HIT_NORMAL)
        .WithSpellTypeMask(PROC_SPELL_TYPE_DAMAGE)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_FINISH)
        .WithDamageInfo(&damageInfo)
        .Build();

    EXPECT_TRUE(sSpellMgr->CanSpellTriggerProcOnEvent(procEntry, eventInfo))
        << "Killing Machine style proc should trigger on FINISH phase with DAMAGE spellTypeMask";
}

/**
 * @brief Verify the bug scenario - FINISH phase with NO_DMG_HEAL fails
 *
 * This test documents the bug behavior: if FINISH phase incorrectly uses
 * NO_DMG_HEAL spellTypeMask, Killing Machine style procs fail.
 */
TEST_F(SpellProcSpellTypeMaskTest, KillingMachine_BugScenario_NoDmgHealFails)
{
    auto procEntry = SpellProcEntryBuilder()
        .WithSpellFamilyName(15)
        .WithSpellFamilyMask(flag96(2, 0, 0))
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellTypeMask(PROC_SPELL_TYPE_DAMAGE) // Requires DAMAGE
        .WithSpellPhaseMask(PROC_SPELL_PHASE_FINISH)
        .Build();

    auto* icyTouchSpell = CreateSpellInfo(49909, 15, 2);
    DamageInfo damageInfo(nullptr, nullptr, 100, icyTouchSpell, SPELL_SCHOOL_MASK_FROST, SPELL_DIRECT_DAMAGE);

    // Simulate the bug: FINISH phase with NO_DMG_HEAL (the old broken behavior)
    auto eventInfo = ProcEventInfoBuilder()
        .WithTypeMask(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithHitMask(PROC_HIT_NORMAL)
        .WithSpellTypeMask(PROC_SPELL_TYPE_NO_DMG_HEAL) // Bug: wrong mask
        .WithSpellPhaseMask(PROC_SPELL_PHASE_FINISH)
        .WithDamageInfo(&damageInfo)
        .Build();

    // This should fail - documenting the bug behavior
    EXPECT_FALSE(sSpellMgr->CanSpellTriggerProcOnEvent(procEntry, eventInfo))
        << "With NO_DMG_HEAL spellTypeMask, DAMAGE-requiring procs should NOT trigger (this was the bug)";
}

TEST_F(SpellProcSpellTypeMaskTest, FinishPhase_StaticClassification_DoesNotTreatSelfBuffAsDamageOrHeal)
{
    auto* selfBuffSpell = SpellInfoBuilder()
        .WithId(700001)
        .WithEffect(EFFECT_0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_STAT)
        .Build();
    _spellInfos.push_back(selfBuffSpell);

    uint32 result = CalculateSpellTypeMask(PROC_SPELL_PHASE_FINISH, nullptr, nullptr, selfBuffSpell);
    EXPECT_EQ(result, PROC_SPELL_TYPE_NO_DMG_HEAL);
}

TEST_F(SpellProcSpellTypeMaskTest, FinishPhase_StaticClassification_TreatsDotCastAsDamageSpell)
{
    auto* dotSpell = SpellInfoBuilder()
        .WithId(700002)
        .WithEffect(EFFECT_0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_PERIODIC_DAMAGE)
        .Build();
    _spellInfos.push_back(dotSpell);

    uint32 result = CalculateSpellTypeMask(PROC_SPELL_PHASE_FINISH, nullptr, nullptr, dotSpell);
    EXPECT_EQ(result, PROC_SPELL_TYPE_DAMAGE);
}

TEST_F(SpellProcSpellTypeMaskTest, Tradeskill_StaticClassification_IsNoDmgHeal)
{
    auto* tradeskillSpell = SpellInfoBuilder()
        .WithId(700013)
        .WithAttributes(SPELL_ATTR0_IS_TRADESKILL)
        .Build();
    _spellInfos.push_back(tradeskillSpell);

    bool reliable = false;
    uint32 result = tradeskillSpell->GetStaticProcSpellTypeMask(reliable);

    EXPECT_TRUE(reliable);
    EXPECT_EQ(result, PROC_SPELL_TYPE_NO_DMG_HEAL);
}

TEST_F(SpellProcSpellTypeMaskTest, PeriodicTick_DoesNotMatchNonPeriodicDamageCastProc)
{
    auto procEntry = SpellProcEntryBuilder()
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellTypeMask(PROC_SPELL_TYPE_DAMAGE)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_FINISH)
        .Build();

    auto* dotSpell = SpellInfoBuilder()
        .WithId(700003)
        .WithEffect(EFFECT_0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_PERIODIC_DAMAGE)
        .Build();
    _spellInfos.push_back(dotSpell);

    DamageInfo periodicTickInfo(nullptr, nullptr, 50, dotSpell, SPELL_SCHOOL_MASK_SHADOW, SPELL_DIRECT_DAMAGE);

    auto eventInfo = ProcEventInfoBuilder()
        .WithTypeMask(PROC_FLAG_DONE_PERIODIC)
        .WithSpellTypeMask(PROC_SPELL_TYPE_DAMAGE)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_HIT)
        .WithHitMask(PROC_HIT_NORMAL)
        .WithDamageInfo(&periodicTickInfo)
        .Build();

    EXPECT_FALSE(sSpellMgr->CanSpellTriggerProcOnEvent(procEntry, eventInfo))
        << "Periodic ticks must not match procs that only listen to spell cast/finish events";
}
