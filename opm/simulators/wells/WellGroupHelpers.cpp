/*
  Copyright 2019 Norce.
  Copyright 2020 Equinor ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>
#include <opm/simulators/wells/WellGroupHelpers.hpp>
#include <opm/simulators/wells/TargetCalculator.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/Group/GConSump.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/Group/GConSale.hpp>
#include <algorithm>
#include <stack>
#include <vector>

namespace {
    Opm::GuideRate::RateVector
    getGuideRateVector(const std::vector<double>& rates, const Opm::PhaseUsage& pu)
    {
        using Opm::BlackoilPhases;

        double oilRate = 0.0;
        if (pu.phase_used[BlackoilPhases::Liquid])
            oilRate = rates[pu.phase_pos[BlackoilPhases::Liquid]];

        double gasRate = 0.0;
        if (pu.phase_used[BlackoilPhases::Vapour])
            gasRate = rates[pu.phase_pos[BlackoilPhases::Vapour]];

        double waterRate = 0.0;
        if (pu.phase_used[BlackoilPhases::Aqua])
            waterRate = rates[pu.phase_pos[BlackoilPhases::Aqua]];

        return {oilRate, gasRate, waterRate};
    }
} // namespace Anonymous

namespace Opm
{


namespace WellGroupHelpers
{


    void setCmodeGroup(const Group& group,
                       const Schedule& schedule,
                       const SummaryState& summaryState,
                       const int reportStepIdx,
                       WellStateFullyImplicitBlackoil& wellState,
                       const GroupState& group_state)
    {

        for (const std::string& groupName : group.groups()) {
            setCmodeGroup(schedule.getGroup(groupName, reportStepIdx), schedule, summaryState, reportStepIdx, wellState, group_state);
        }

        // use NONE as default control
        const Phase all[] = {Phase::WATER, Phase::OIL, Phase::GAS};
        for (Phase phase : all) {
            if (!wellState.hasInjectionGroupControl(phase, group.name())) {
                wellState.setCurrentInjectionGroupControl(phase, group.name(), Group::InjectionCMode::NONE);
            }
        }
        if (!group_state.has_production_control(group.name())) {
            wellState.setCurrentProductionGroupControl(group.name(), Group::ProductionCMode::NONE);
        }

        const auto& events = schedule[reportStepIdx].wellgroup_events();
        if (group.isInjectionGroup()
            && events.hasEvent(group.name(), ScheduleEvents::GROUP_INJECTION_UPDATE)) {

            for (Phase phase : all) {
                if (!group.hasInjectionControl(phase))
                    continue;

                const auto& controls = group.injectionControls(phase, summaryState);
                wellState.setCurrentInjectionGroupControl(phase, group.name(), controls.cmode);
            }
        }

        if (group.isProductionGroup()
            && events.hasEvent(group.name(), ScheduleEvents::GROUP_PRODUCTION_UPDATE)) {
            const auto controls = group.productionControls(summaryState);
            wellState.setCurrentProductionGroupControl(group.name(), controls.cmode);
        }

        if (schedule[reportStepIdx].gconsale().has(group.name())) {
            wellState.setCurrentInjectionGroupControl(Phase::GAS, group.name(), Group::InjectionCMode::SALE);
        }
    }

    void accumulateGroupEfficiencyFactor(const Group& group,
                                         const Schedule& schedule,
                                         const int reportStepIdx,
                                         double& factor)
    {
        factor *= group.getGroupEfficiencyFactor();
        if (group.parent() != "FIELD")
            accumulateGroupEfficiencyFactor(
                schedule.getGroup(group.parent(), reportStepIdx), schedule, reportStepIdx, factor);
    }

    double sumWellPhaseRates(const std::vector<double>& rates,
                             const Group& group,
                             const Schedule& schedule,
                             const WellStateFullyImplicitBlackoil& wellState,
                             const int reportStepIdx,
                             const int phasePos,
                             const bool injector)
    {

        double rate = 0.0;
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            rate += sumWellPhaseRates(rates, groupTmp, schedule, wellState, reportStepIdx, phasePos, injector);
        }
        const auto& end = wellState.wellMap().end();

        for (const std::string& wellName : group.wells()) {
            const auto& it = wellState.wellMap().find(wellName);
            if (it == end) // the well is not found
                continue;

            int well_index = it->second[0];

            if (! wellState.wellIsOwned(well_index, wellName) ) // Only sum once
            {
                continue;
            }

            const auto& wellEcl = schedule.getWell(wellName, reportStepIdx);
            // only count producers or injectors
            if ((wellEcl.isProducer() && injector) || (wellEcl.isInjector() && !injector))
                continue;

            if (wellEcl.getStatus() == Well::Status::SHUT)
                continue;

            double factor = wellEcl.getEfficiencyFactor();
            const auto wellrate_index = well_index * wellState.numPhases();
            if (injector)
                rate += factor * rates[wellrate_index + phasePos];
            else
                rate -= factor * rates[wellrate_index + phasePos];
        }
        const auto& gefac = group.getGroupEfficiencyFactor();
        return gefac * rate;
    }

    double sumWellRates(const Group& group,
                        const Schedule& schedule,
                        const WellStateFullyImplicitBlackoil& wellState,
                        const int reportStepIdx,
                        const int phasePos,
                        const bool injector)
    {
        return sumWellPhaseRates(wellState.wellRates(), group, schedule, wellState, reportStepIdx, phasePos, injector);
    }

    double sumWellResRates(const Group& group,
                           const Schedule& schedule,
                           const WellStateFullyImplicitBlackoil& wellState,
                           const int reportStepIdx,
                           const int phasePos,
                           const bool injector)
    {
        return sumWellPhaseRates(
            wellState.wellReservoirRates(), group, schedule, wellState, reportStepIdx, phasePos, injector);
    }

    double sumSolventRates(const Group& group,
                           const Schedule& schedule,
                           const WellStateFullyImplicitBlackoil& wellState,
                           const int reportStepIdx,
                           const bool injector)
    {

        double rate = 0.0;
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            rate += sumSolventRates(groupTmp, schedule, wellState, reportStepIdx, injector);
        }
        const auto& end = wellState.wellMap().end();
        for (const std::string& wellName : group.wells()) {
            const auto& it = wellState.wellMap().find(wellName);
            if (it == end) // the well is not found
                continue;

            int well_index = it->second[0];

            if (! wellState.wellIsOwned(well_index, wellName) ) // Only sum once
            {
                continue;
            }

            const auto& wellEcl = schedule.getWell(wellName, reportStepIdx);
            // only count producers or injectors
            if ((wellEcl.isProducer() && injector) || (wellEcl.isInjector() && !injector))
                continue;

            if (wellEcl.getStatus() == Well::Status::SHUT)
                continue;

            double factor = wellEcl.getEfficiencyFactor();
            if (injector)
                rate += factor * wellState.solventWellRate(well_index);
            else
                rate -= factor * wellState.solventWellRate(well_index);
        }
        const auto& gefac = group.getGroupEfficiencyFactor();
        return gefac * rate;
    }

    void updateGuideRatesForInjectionGroups(const Group& group,
                                            const Schedule& schedule,
                                            const SummaryState& summaryState,
                                            const Opm::PhaseUsage& pu,
                                            const int reportStepIdx,
                                            const WellStateFullyImplicitBlackoil& wellState,
                                            GuideRate* guideRate,
                                            Opm::DeferredLogger& deferred_logger)
    {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateGuideRatesForInjectionGroups(
                        groupTmp, schedule, summaryState, pu, reportStepIdx, wellState, guideRate, deferred_logger);
        }
        const Phase all[] = {Phase::WATER, Phase::OIL, Phase::GAS};
        for (Phase phase : all) {

            if(!group.hasInjectionControl(phase))
                continue;

            double guideRateValue = 0.0;
            const auto& controls = group.injectionControls(phase, summaryState);
            switch (controls.guide_rate_def){
            case Group::GuideRateInjTarget::RATE:
                break;
            case Group::GuideRateInjTarget::VOID:
            {
                guideRateValue = wellState.currentInjectionVREPRates(group.name());
                break;
            }
            case Group::GuideRateInjTarget::NETV:
            {
                guideRateValue = wellState.currentInjectionVREPRates(group.name());
                const std::vector<double>& injRES = wellState.currentInjectionGroupReservoirRates(group.name());
                if (phase != Phase::OIL && pu.phase_used[BlackoilPhases::Liquid])
                    guideRateValue -= injRES[pu.phase_pos[BlackoilPhases::Liquid]];
                if (phase != Phase::GAS && pu.phase_used[BlackoilPhases::Vapour])
                    guideRateValue -= injRES[pu.phase_pos[BlackoilPhases::Vapour]];
                if (phase != Phase::WATER && pu.phase_used[BlackoilPhases::Aqua])
                    guideRateValue -= injRES[pu.phase_pos[BlackoilPhases::Aqua]];
                break;
            }
            case Group::GuideRateInjTarget::RESV:
                OPM_DEFLOG_THROW(std::runtime_error, "GUIDE PHASE RESV not implemented. Group " + group.name(), deferred_logger);
            case Group::GuideRateInjTarget::POTN:
                break;
            case Group::GuideRateInjTarget::NO_GUIDE_RATE:
                break;
            default:
                OPM_DEFLOG_THROW(std::logic_error,
                                 "Invalid GuideRateInjTarget in updateGuideRatesForInjectionGroups",
                                 deferred_logger);
            }
            guideRate->compute(group.name(), phase, reportStepIdx, guideRateValue);
        }
    }

    void updateGroupTargetReduction(const Group& group,
                                    const Schedule& schedule,
                                    const int reportStepIdx,
                                    const bool isInjector,
                                    const PhaseUsage& pu,
                                    const GuideRate& guide_rate,
                                    const WellStateFullyImplicitBlackoil& wellStateNupcol,
                                    WellStateFullyImplicitBlackoil& wellState,
                                    std::vector<double>& groupTargetReduction)
    {
        const int np = wellState.numPhases();
        for (const std::string& subGroupName : group.groups()) {
            std::vector<double> subGroupTargetReduction(np, 0.0);
            const Group& subGroup = schedule.getGroup(subGroupName, reportStepIdx);
            updateGroupTargetReduction(subGroup,
                                       schedule,
                                       reportStepIdx,
                                       isInjector,
                                       pu,
                                       guide_rate,
                                       wellStateNupcol,
                                       wellState,
                                       subGroupTargetReduction);

            // accumulate group contribution from sub group
            if (isInjector) {
                const Phase all[] = {Phase::WATER, Phase::OIL, Phase::GAS};
                for (Phase phase : all) {
                    const Group::InjectionCMode& currentGroupControl
                        = wellState.currentInjectionGroupControl(phase, subGroupName);
                    int phasePos;
                    if (phase == Phase::GAS && pu.phase_used[BlackoilPhases::Vapour])
                        phasePos = pu.phase_pos[BlackoilPhases::Vapour];
                    else if (phase == Phase::OIL && pu.phase_used[BlackoilPhases::Liquid])
                        phasePos = pu.phase_pos[BlackoilPhases::Liquid];
                    else if (phase == Phase::WATER && pu.phase_used[BlackoilPhases::Aqua])
                        phasePos = pu.phase_pos[BlackoilPhases::Aqua];
                    else
                        continue;

                    if (currentGroupControl != Group::InjectionCMode::FLD
                        && currentGroupControl != Group::InjectionCMode::NONE) {
                        // Subgroup is under individual control.
                        groupTargetReduction[phasePos]
                            += sumWellRates(subGroup, schedule, wellStateNupcol, reportStepIdx, phasePos, isInjector);
                    } else {
                        groupTargetReduction[phasePos] += subGroupTargetReduction[phasePos];
                    }
                }
            } else {
                const Group::ProductionCMode& currentGroupControl
                    = wellState.currentProductionGroupControl(subGroupName);
                const bool individual_control = (currentGroupControl != Group::ProductionCMode::FLD
                                                 && currentGroupControl != Group::ProductionCMode::NONE);
                const int num_group_controlled_wells
                    = groupControlledWells(schedule, wellStateNupcol, reportStepIdx, subGroupName, "", !isInjector, /*injectionPhaseNotUsed*/Phase::OIL);
                if (individual_control || num_group_controlled_wells == 0) {
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase]
                            += sumWellRates(subGroup, schedule, wellStateNupcol, reportStepIdx, phase, isInjector);
                    }
                } else {
                    // The subgroup may participate in group control.
                    if (!guide_rate.has(subGroupName)) {
                        // Accumulate from this subgroup only if no group guide rate is set for it.
                        for (int phase = 0; phase < np; phase++) {
                            groupTargetReduction[phase] += subGroupTargetReduction[phase];
                        }
                    }
                }
            }
        }

        for (const std::string& wellName : group.wells()) {
            const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);

            if (wellTmp.isProducer() && isInjector)
                continue;

            if (wellTmp.isInjector() && !isInjector)
                continue;

            if (wellTmp.getStatus() == Well::Status::SHUT)
                continue;

            const auto& end = wellState.wellMap().end();
            const auto& it = wellState.wellMap().find(wellName);
            if (it == end) // the well is not found
                continue;

            int well_index = it->second[0];

            if (! wellState.wellIsOwned(well_index, wellName) ) // Only sum once
            {
                continue;
            }

            const auto wellrate_index = well_index * wellState.numPhases();
            const double efficiency = wellTmp.getEfficiencyFactor();
            // add contributino from wells not under group control
            if (isInjector) {
                if (wellState.currentInjectionControls()[well_index] != Well::InjectorCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] += wellStateNupcol.wellRates()[wellrate_index + phase] * efficiency;
                    }
            } else {
                if (wellState.currentProductionControls()[well_index] != Well::ProducerCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] -= wellStateNupcol.wellRates()[wellrate_index + phase] * efficiency;
                    }
            }
        }
        const double groupEfficiency = group.getGroupEfficiencyFactor();
        for (double& elem : groupTargetReduction) {
            elem *= groupEfficiency;
        }
        if (isInjector)
            wellState.setCurrentInjectionGroupReductionRates(group.name(), groupTargetReduction);
        else
            wellState.setCurrentProductionGroupReductionRates(group.name(), groupTargetReduction);
    }


    void updateVREPForGroups(const Group& group,
                             const Schedule& schedule,
                             const int reportStepIdx,
                             const WellStateFullyImplicitBlackoil& wellStateNupcol,
                             WellStateFullyImplicitBlackoil& wellState)
    {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateVREPForGroups(groupTmp, schedule, reportStepIdx, wellStateNupcol, wellState);
        }
        const int np = wellState.numPhases();
        double resv = 0.0;
        for (int phase = 0; phase < np; ++phase) {
            resv += sumWellPhaseRates(wellStateNupcol.wellReservoirRates(),
                                      group,
                                      schedule,
                                      wellState,
                                      reportStepIdx,
                                      phase,
                                      /*isInjector*/ false);
        }
        wellState.setCurrentInjectionVREPRates(group.name(), resv);
    }

    void updateReservoirRatesInjectionGroups(const Group& group,
                                             const Schedule& schedule,
                                             const int reportStepIdx,
                                             const WellStateFullyImplicitBlackoil& wellStateNupcol,
                                             WellStateFullyImplicitBlackoil& wellState)
    {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateReservoirRatesInjectionGroups(groupTmp, schedule, reportStepIdx, wellStateNupcol, wellState);
        }
        const int np = wellState.numPhases();
        std::vector<double> resv(np, 0.0);
        for (int phase = 0; phase < np; ++phase) {
            resv[phase] = sumWellPhaseRates(wellStateNupcol.wellReservoirRates(),
                                            group,
                                            schedule,
                                            wellState,
                                            reportStepIdx,
                                            phase,
                                            /*isInjector*/ true);
        }
        wellState.setCurrentInjectionGroupReservoirRates(group.name(), resv);
    }

    void updateWellRates(const Group& group,
                         const Schedule& schedule,
                         const int reportStepIdx,
                         const WellStateFullyImplicitBlackoil& wellStateNupcol,
                         WellStateFullyImplicitBlackoil& wellState)
    {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateWellRates(groupTmp, schedule, reportStepIdx, wellStateNupcol, wellState);
        }
        const int np = wellState.numPhases();
        const auto& end = wellState.wellMap().end();
        for (const std::string& wellName : group.wells()) {
            std::vector<double> rates(np, 0.0);
            const auto& it = wellState.wellMap().find(wellName);
            if (it != end) { // the well is found on this node
                int well_index = it->second[0];
                const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);
                int sign = 1;
                // production wellRates are negative. The users of currentWellRates uses the convention in
                // opm-common that production and injection rates are positive.
                if (!wellTmp.isInjector())
                    sign = -1;
                for (int phase = 0; phase < np; ++phase) {
                    rates[phase] = sign * wellStateNupcol.wellRates()[well_index * np + phase];
                }
            }
            wellState.setCurrentWellRates(wellName, rates);
        }
    }

    void updateGroupProductionRates(const Group& group,
                                    const Schedule& schedule,
                                    const int reportStepIdx,
                                    const WellStateFullyImplicitBlackoil& wellStateNupcol,
                                    WellStateFullyImplicitBlackoil& wellState)
    {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateGroupProductionRates(groupTmp, schedule, reportStepIdx, wellStateNupcol, wellState);
        }
        const int np = wellState.numPhases();
        std::vector<double> rates(np, 0.0);
        for (int phase = 0; phase < np; ++phase) {
            rates[phase] = sumWellPhaseRates(
                wellStateNupcol.wellRates(), group, schedule, wellState, reportStepIdx, phase, /*isInjector*/ false);
        }
        wellState.setCurrentProductionGroupRates(group.name(), rates);
    }


    void updateREINForGroups(const Group& group,
                             const Schedule& schedule,
                             const int reportStepIdx,
                             const PhaseUsage& pu,
                             const SummaryState& st,
                             const WellStateFullyImplicitBlackoil& wellStateNupcol,
                             WellStateFullyImplicitBlackoil& wellState)
    {
        const int np = wellState.numPhases();
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateREINForGroups(groupTmp, schedule, reportStepIdx, pu, st, wellStateNupcol, wellState);
        }

        std::vector<double> rein(np, 0.0);
        for (int phase = 0; phase < np; ++phase) {
            rein[phase] = sumWellPhaseRates(
                wellStateNupcol.wellRates(), group, schedule, wellState, reportStepIdx, phase, /*isInjector*/ false);
        }

        // add import rate and substract consumption rate for group for gas
        if (schedule[reportStepIdx].gconsump().has(group.name())) {
            const auto& gconsump = schedule[reportStepIdx].gconsump().get(group.name(), st);
            if (pu.phase_used[BlackoilPhases::Vapour]) {
                rein[pu.phase_pos[BlackoilPhases::Vapour]] += gconsump.import_rate;
                rein[pu.phase_pos[BlackoilPhases::Vapour]] -= gconsump.consumption_rate;
            }
        }

        wellState.setCurrentInjectionREINRates(group.name(), rein);
    }




    std::map<std::string, double>
    computeNetworkPressures(const Opm::Network::ExtNetwork& network,
                            const WellStateFullyImplicitBlackoil& well_state,
                            const VFPProdProperties& vfp_prod_props,
                            const Schedule& schedule,
                            const int report_time_step)
    {
        // TODO: Only dealing with production networks for now.

        if (!network.active()) {
            return {};
        }

        // Fixed pressure nodes of the network are the roots of trees.
        // Leaf nodes must correspond to groups in the group structure.
        // Let us first find all leaf nodes of the network. We also
        // create a vector of all nodes, ordered so that a child is
        // always after its parent.
        std::stack<std::string> children;
        std::set<std::string> leaf_nodes;
        std::vector<std::string> root_to_child_nodes;
        children.push(network.root().name());
        while (!children.empty()) {
            const auto node = children.top();
            children.pop();
            root_to_child_nodes.push_back(node);
            auto branches = network.downtree_branches(node);
            if (branches.empty()) {
                leaf_nodes.insert(node);
            }
            for (const auto& branch : branches) {
                children.push(branch.downtree_node());
            }
        }
        assert(children.empty());

        // Starting with the leaf nodes of the network, get the flow rates
        // from the corresponding groups.
        std::map<std::string, std::vector<double>> node_inflows;
        for (const auto& node : leaf_nodes) {
            node_inflows[node] = well_state.currentProductionGroupRates(node);
            // Add the ALQ amounts to the gas rates if requested.
            if (network.node(node).add_gas_lift_gas()) {
                const auto& group = schedule.getGroup(node, report_time_step);
                for (const std::string& wellname : group.wells()) {
                    node_inflows[node][BlackoilPhases::Vapour] += well_state.getALQ(wellname);
                }
            }
        }

        // Accumulate in the network, towards the roots. Note that a
        // root (i.e. fixed pressure node) can still be contributing
        // flow towards other nodes in the network, i.e.  a node is
        // the root of a subtree.
        auto child_to_root_nodes = root_to_child_nodes;
        std::reverse(child_to_root_nodes.begin(), child_to_root_nodes.end());
        for (const auto& node : child_to_root_nodes) {
            const auto upbranch = network.uptree_branch(node);
            if (upbranch) {
                // Add downbranch rates to upbranch.
                std::vector<double>& up = node_inflows[(*upbranch).uptree_node()];
                const std::vector<double>& down = node_inflows[node];
                if (up.empty()) {
                    up = down;
                } else {
                    assert (up.size() == down.size());
                    for (size_t ii = 0; ii < up.size(); ++ii) {
                        up[ii] += down[ii];
                    }
                }
            }
        }

        // Going the other way (from roots to leafs), calculate the pressure
        // at each node using VFP tables and rates.
        std::map<std::string, double> node_pressures;
        for (const auto& node : root_to_child_nodes) {
            auto press = network.node(node).terminal_pressure();
            if (press) {
                node_pressures[node] = *press;
            } else {
                const auto upbranch = network.uptree_branch(node);
                assert(upbranch);
                const double up_press = node_pressures[(*upbranch).uptree_node()];
                const auto vfp_table = (*upbranch).vfp_table();
                if (vfp_table) {
                    // The rates are here positive, but the VFP code expects the
                    // convention that production rates are negative, so we must
                    // take a copy and flip signs.
                    auto rates = node_inflows[node];
                    for (auto& r : rates) { r *= -1.0; }
                    assert(rates.size() == 3);
                    const double alq = 0.0; // TODO: Do not ignore ALQ
                    node_pressures[node] = vfp_prod_props.bhp(*vfp_table,
                                                              rates[BlackoilPhases::Aqua],
                                                              rates[BlackoilPhases::Liquid],
                                                              rates[BlackoilPhases::Vapour],
                                                              up_press,
                                                              alq);
#define EXTRA_DEBUG_NETWORK 0
#if EXTRA_DEBUG_NETWORK
                    std::ostringstream oss;
                    oss << "parent: " << (*upbranch).uptree_node() << "  child: " << node
                        << "  rates = [ " << rates[0]*86400 << ", " << rates[1]*86400 << ", " << rates[2]*86400 << " ]"
                        << "  p(parent) = " << up_press/1e5 << "  p(child) = " << node_pressures[node]/1e5 << std::endl;
                    OpmLog::debug(oss.str());
#endif
                } else {
                    // Table number specified as 9999 in the deck, no pressure loss.
                    node_pressures[node] = up_press;
                }
            }
        }

        return node_pressures;
    }




    GuideRate::RateVector
    getWellRateVector(const WellStateFullyImplicitBlackoil& well_state, const PhaseUsage& pu, const std::string& name)
    {
        return getGuideRateVector(well_state.currentWellRates(name), pu);
    }

    GuideRate::RateVector
    getProductionGroupRateVector(const WellStateFullyImplicitBlackoil& well_state, const PhaseUsage& pu, const std::string& group_name)
    {
        return getGuideRateVector(well_state.currentProductionGroupRates(group_name), pu);
    }

    double getGuideRate(const std::string& name,
                        const Schedule& schedule,
                        const WellStateFullyImplicitBlackoil& wellState,
                        const int reportStepIdx,
                        const GuideRate* guideRate,
                        const GuideRateModel::Target target,
                        const PhaseUsage& pu)
    {
        if (schedule.hasWell(name, reportStepIdx)) {
            return guideRate->get(name, target, getWellRateVector(wellState, pu, name));
        }

        if (guideRate->has(name)) {
            return guideRate->get(name, target, getProductionGroupRateVector(wellState, pu, name));
        }

        double totalGuideRate = 0.0;
        const Group& group = schedule.getGroup(name, reportStepIdx);

        for (const std::string& groupName : group.groups()) {
            const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(groupName);
            if (currentGroupControl == Group::ProductionCMode::FLD
                || currentGroupControl == Group::ProductionCMode::NONE) {
                // accumulate from sub wells/groups
                totalGuideRate += getGuideRate(groupName, schedule, wellState, reportStepIdx, guideRate, target, pu);
            }
        }

        for (const std::string& wellName : group.wells()) {
            const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);

            if (wellTmp.isInjector())
                continue;

            if (wellTmp.getStatus() == Well::Status::SHUT)
                continue;

            // Only count wells under group control or the ru
            if (!wellState.isProductionGrup(wellName))
                continue;

            totalGuideRate += guideRate->get(wellName, target, getWellRateVector(wellState, pu, wellName));
        }
        return totalGuideRate;
    }


    double getGuideRateInj(const std::string& name,
                           const Schedule& schedule,
                           const WellStateFullyImplicitBlackoil& wellState,
                           const int reportStepIdx,
                           const GuideRate* guideRate,
                           const GuideRateModel::Target target,
                           const Phase& injectionPhase,
                           const PhaseUsage& pu)
    {
        if (schedule.hasWell(name, reportStepIdx)) {
            return guideRate->get(name, target, getWellRateVector(wellState, pu, name));
        }

        if (guideRate->has(name, injectionPhase)) {
            return guideRate->get(name, injectionPhase);
        }

        double totalGuideRate = 0.0;
        const Group& group = schedule.getGroup(name, reportStepIdx);

        for (const std::string& groupName : group.groups()) {
            const Group::InjectionCMode& currentGroupControl
                = wellState.currentInjectionGroupControl(injectionPhase, groupName);
            if (currentGroupControl == Group::InjectionCMode::FLD
                || currentGroupControl == Group::InjectionCMode::NONE) {
                // accumulate from sub wells/groups
                totalGuideRate += getGuideRateInj(
                    groupName, schedule, wellState, reportStepIdx, guideRate, target, injectionPhase, pu);
            }
        }

        for (const std::string& wellName : group.wells()) {
            const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);

            if (!wellTmp.isInjector())
                continue;

            if (wellTmp.getStatus() == Well::Status::SHUT)
                continue;

            // Only count wells under group control or the ru
            if (!wellState.isInjectionGrup(wellName))
                continue;

            totalGuideRate += guideRate->get(wellName, target, getWellRateVector(wellState, pu, wellName));
        }
        return totalGuideRate;
    }



    int groupControlledWells(const Schedule& schedule,
                             const WellStateFullyImplicitBlackoil& well_state,
                             const int report_step,
                             const std::string& group_name,
                             const std::string& always_included_child,
                             const bool is_production_group,
                             const Phase injection_phase)
    {
        const Group& group = schedule.getGroup(group_name, report_step);
        int num_wells = 0;
        for (const std::string& child_group : group.groups()) {

            bool included = (child_group == always_included_child);
            if (is_production_group) {
                const auto ctrl = well_state.currentProductionGroupControl(child_group);
                included = included || (ctrl == Group::ProductionCMode::FLD) || (ctrl == Group::ProductionCMode::NONE);
            } else {
                const auto ctrl = well_state.currentInjectionGroupControl(injection_phase, child_group);
                included = included || (ctrl == Group::InjectionCMode::FLD) || (ctrl == Group::InjectionCMode::NONE);
            }

            if (included) {
                num_wells
                    += groupControlledWells(schedule, well_state, report_step, child_group, always_included_child, is_production_group, injection_phase);
            }
        }
        for (const std::string& child_well : group.wells()) {
            bool included = (child_well == always_included_child);
            if (is_production_group) {
                included = included || well_state.isProductionGrup(child_well);
            } else {
                included = included || well_state.isInjectionGrup(child_well);
            }
            if (included) {
                ++num_wells;
            }
        }
        return num_wells;
    }

    FractionCalculator::FractionCalculator(const Schedule& schedule,
                                           const SummaryState& summary_state,
                                           const WellStateFullyImplicitBlackoil& well_state,
                                           const int report_step,
                                           const GuideRate* guide_rate,
                                           const GuideRateModel::Target target,
                                           const PhaseUsage& pu,
                                           const bool is_producer,
                                           const Phase injection_phase)
        : schedule_(schedule)
        , summary_state_(summary_state)
        , well_state_(well_state)
        , report_step_(report_step)
        , guide_rate_(guide_rate)
        , target_(target)
        , pu_(pu)
        , is_producer_(is_producer)
        , injection_phase_(injection_phase)
    {
    }
    double FractionCalculator::fraction(const std::string& name,
                                        const std::string& control_group_name,
                                        const bool always_include_this)
    {
        double fraction = 1.0;
        std::string current = name;
        while (current != control_group_name) {
            fraction *= localFraction(current, always_include_this ? name : "");
            current = parent(current);
        }
        return fraction;
    }
    double FractionCalculator::localFraction(const std::string& name, const std::string& always_included_child)
    {
        const double my_guide_rate = guideRate(name, always_included_child);
        const Group& parent_group = schedule_.getGroup(parent(name), report_step_);
        const double total_guide_rate = guideRateSum(parent_group, always_included_child);
        assert(total_guide_rate >= my_guide_rate);
        const double guide_rate_epsilon = 1e-12;
        return (total_guide_rate > guide_rate_epsilon) ? my_guide_rate / total_guide_rate : 0.0;
    }
    std::string FractionCalculator::parent(const std::string& name)
    {
        if (schedule_.hasWell(name)) {
            return schedule_.getWell(name, report_step_).groupName();
        } else {
            return schedule_.getGroup(name, report_step_).parent();
        }
    }
    double FractionCalculator::guideRateSum(const Group& group, const std::string& always_included_child)
    {
        double total_guide_rate = 0.0;
        for (const std::string& child_group : group.groups()) {
            bool included = (child_group == always_included_child);
            if (is_producer_) {
                const auto ctrl = well_state_.currentProductionGroupControl(child_group);
                included = included || (ctrl == Group::ProductionCMode::FLD) || (ctrl == Group::ProductionCMode::NONE);
            } else {
                const auto ctrl = well_state_.currentInjectionGroupControl(injection_phase_, child_group);
                included = included || (ctrl == Group::InjectionCMode::FLD) || (ctrl == Group::InjectionCMode::NONE);
            }
            if (included) {
                total_guide_rate += guideRate(child_group, always_included_child);
            }
        }
        for (const std::string& child_well : group.wells()) {
            bool included = (child_well == always_included_child);
            if (is_producer_) {
                included = included || well_state_.isProductionGrup(child_well);
            } else {
                included = included || well_state_.isInjectionGrup(child_well);
            }

            if (included) {
                total_guide_rate += guideRate(child_well, always_included_child);
            }
        }
        return total_guide_rate;
    }
    double FractionCalculator::guideRate(const std::string& name, const std::string& always_included_child)
    {
        if (schedule_.hasWell(name, report_step_)) {
            return guide_rate_->get(name, target_, getWellRateVector(well_state_, pu_, name));
        } else {
            if (groupControlledWells(name, always_included_child) > 0) {
                if (is_producer_ && guide_rate_->has(name)) {
                    return guide_rate_->get(name, target_, getGroupRateVector(name));
                } else if (!is_producer_ && guide_rate_->has(name, injection_phase_)) {
                    return guide_rate_->get(name, injection_phase_);
                } else {
                    // We are a group, with default guide rate.
                    // Compute guide rate by accumulating our children's guide rates.
                    const Group& group = schedule_.getGroup(name, report_step_);
                    return guideRateSum(group, always_included_child);
                }
            } else {
                // No group-controlled subordinate wells.
                return 0.0;
            }
        }
    }
    int FractionCalculator::groupControlledWells(const std::string& group_name,
                                                 const std::string& always_included_child)
    {
        return ::Opm::WellGroupHelpers::groupControlledWells(
            schedule_, well_state_, report_step_, group_name, always_included_child, is_producer_, injection_phase_);
    }

    GuideRate::RateVector FractionCalculator::getGroupRateVector(const std::string& group_name)
    {
        assert(is_producer_);
        return getProductionGroupRateVector(this->well_state_, this->pu_, group_name);
    }


    std::vector<std::string>
    groupChainTopBot(const std::string& bottom, const std::string& top, const Schedule& schedule, const int report_step)
    {
        // Get initial parent, 'bottom' can be a well or a group.
        std::string parent;
        if (schedule.hasWell(bottom, report_step)) {
            parent = schedule.getWell(bottom, report_step).groupName();
        } else {
            parent = schedule.getGroup(bottom, report_step).parent();
        }

        // Build the chain from bottom to top.
        std::vector<std::string> chain;
        chain.push_back(bottom);
        chain.push_back(parent);
        while (parent != top) {
            parent = schedule.getGroup(parent, report_step).parent();
            chain.push_back(parent);
        }
        assert(chain.back() == top);

        // Reverse order and return.
        std::reverse(chain.begin(), chain.end());
        return chain;
    }




    std::pair<bool, double> checkGroupConstraintsProd(const std::string& name,
                                                      const std::string& parent,
                                                      const Group& group,
                                                      const WellStateFullyImplicitBlackoil& wellState,
                                                      const int reportStepIdx,
                                                      const GuideRate* guideRate,
                                                      const double* rates,
                                                      const PhaseUsage& pu,
                                                      const double efficiencyFactor,
                                                      const Schedule& schedule,
                                                      const SummaryState& summaryState,
                                                      const std::vector<double>& resv_coeff,
                                                      DeferredLogger& deferred_logger)
    {
        // When called for a well ('name' is a well name), 'parent'
        // will be the name of 'group'. But if we recurse, 'name' and
        // 'parent' will stay fixed while 'group' will be higher up
        // in the group tree.
        // efficiencyfactor is the well efficiency factor for the first group the well is
        // part of. Later it is the accumulated factor including the group efficiency factor
        // of the child of group.

        const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(group.name());

        if (currentGroupControl == Group::ProductionCMode::FLD || currentGroupControl == Group::ProductionCMode::NONE) {
            // Return if we are not available for parent group.
            if (!group.productionGroupControlAvailable()) {
                return std::make_pair(false, 1);
            }
            // Otherwise: check production share of parent's control.
            const auto& parentGroup = schedule.getGroup(group.parent(), reportStepIdx);
            return checkGroupConstraintsProd(name,
                                             parent,
                                             parentGroup,
                                             wellState,
                                             reportStepIdx,
                                             guideRate,
                                             rates,
                                             pu,
                                             efficiencyFactor * group.getGroupEfficiencyFactor(),
                                             schedule,
                                             summaryState,
                                             resv_coeff,
                                             deferred_logger);
        }

        // This can be false for FLD-controlled groups, we must therefore
        // check for FLD first (done above).
        if (!group.isProductionGroup()) {
            return std::make_pair(false, 1.0);
        }

        // If we are here, we are at the topmost group to be visited in the recursion.
        // This is the group containing the control we will check against.

        // gconsale may adjust the grat target.
        // the adjusted rates is send to the targetCalculator
        double gratTargetFromSales = 0.0;
        if (wellState.hasGroupGratTargetFromSales(group.name()))
            gratTargetFromSales = wellState.currentGroupGratTargetFromSales(group.name());

        TargetCalculator tcalc(currentGroupControl, pu, resv_coeff, gratTargetFromSales);
        FractionCalculator fcalc(schedule, summaryState, wellState, reportStepIdx, guideRate, tcalc.guideTargetMode(), pu, true, Phase::OIL);

        auto localFraction = [&](const std::string& child) { return fcalc.localFraction(child, name); };

        auto localReduction = [&](const std::string& group_name) {
            const std::vector<double>& groupTargetReductions
                = wellState.currentProductionGroupReductionRates(group_name);
            return tcalc.calcModeRateFromRates(groupTargetReductions);
        };

        const double orig_target = tcalc.groupTarget(group.productionControls(summaryState));
        // Assume we have a chain of groups as follows: BOTTOM -> MIDDLE -> TOP.
        // Then ...
        // TODO finish explanation.
        const double current_rate
            = -tcalc.calcModeRateFromRates(rates); // Switch sign since 'rates' are negative for producers.
        const auto chain = groupChainTopBot(name, group.name(), schedule, reportStepIdx);
        // Because 'name' is the last of the elements, and not an ancestor, we subtract one below.
        const size_t num_ancestors = chain.size() - 1;
        // we need to find out the level where the current well is applied to the local reduction 
        size_t local_reduction_level = 0;
        for (size_t ii = 0; ii < num_ancestors; ++ii) {
            if ((ii == 0) || guideRate->has(chain[ii])) {
                local_reduction_level = ii;
            }
        }

        double efficiencyFactorInclGroup = efficiencyFactor * group.getGroupEfficiencyFactor();
        double target = orig_target;
        for (size_t ii = 0; ii < num_ancestors; ++ii) {
            if ((ii == 0) || guideRate->has(chain[ii])) {
                // Apply local reductions only at the control level
                // (top) and for levels where we have a specified
                // group guide rate.
                target -= localReduction(chain[ii]);

                // Add my reduction back at the level where it is included in the local reduction
                if (local_reduction_level == ii )
                    target += current_rate * efficiencyFactorInclGroup;
            }
            if (ii < num_ancestors - 1) {
                // Not final level. Add sub-level reduction back, if
                // it was nonzero due to having no group-controlled
                // wells.  Note that we make this call without setting
                // the current well to be always included, because we
                // want to know the situation that applied to the
                // calculation of reductions.
                const int num_gr_ctrl = groupControlledWells(schedule, wellState, reportStepIdx, chain[ii + 1], "", /*is_producer*/true, /*injectionPhaseNotUsed*/Phase::OIL);
                if (num_gr_ctrl == 0) {
                    if (guideRate->has(chain[ii + 1])) {
                        target += localReduction(chain[ii + 1]);
                    }
                }
            }
            target *= localFraction(chain[ii + 1]);
        }
        // Avoid negative target rates comming from too large local reductions.
        const double target_rate = std::max(1e-12, target / efficiencyFactorInclGroup);
        return std::make_pair(current_rate > target_rate, target_rate / current_rate);
    }

    std::pair<bool, double> checkGroupConstraintsInj(const std::string& name,
                                                     const std::string& parent,
                                                     const Group& group,
                                                     const WellStateFullyImplicitBlackoil& wellState,
                                                     const int reportStepIdx,
                                                     const GuideRate* guideRate,
                                                     const double* rates,
                                                     Phase injectionPhase,
                                                     const PhaseUsage& pu,
                                                     const double efficiencyFactor,
                                                     const Schedule& schedule,
                                                     const SummaryState& summaryState,
                                                     const std::vector<double>& resv_coeff,
                                                     DeferredLogger& deferred_logger)
    {
        // When called for a well ('name' is a well name), 'parent'
        // will be the name of 'group'. But if we recurse, 'name' and
        // 'parent' will stay fixed while 'group' will be higher up
        // in the group tree.
        // efficiencyfactor is the well efficiency factor for the first group the well is
        // part of. Later it is the accumulated factor including the group efficiency factor
        // of the child of group.

        const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(injectionPhase, group.name());

        if (currentGroupControl == Group::InjectionCMode::FLD || currentGroupControl == Group::InjectionCMode::NONE) {
            // Return if we are not available for parent group.
            if (!group.injectionGroupControlAvailable(injectionPhase)) {
                return std::make_pair(false, 1);
            }
            // Otherwise: check production share of parent's control.
            const auto& parentGroup = schedule.getGroup(group.parent(), reportStepIdx);
            return checkGroupConstraintsInj(name,
                                             parent,
                                             parentGroup,
                                             wellState,
                                             reportStepIdx,
                                             guideRate,
                                             rates,
                                             injectionPhase,
                                             pu,
                                             efficiencyFactor * group.getGroupEfficiencyFactor(),
                                             schedule,
                                             summaryState,
                                             resv_coeff,
                                             deferred_logger);
        }

        // This can be false for FLD-controlled groups, we must therefore
        // check for FLD first (done above).
        if (!group.isInjectionGroup()) {
            return std::make_pair(false, 1.0);
        }

        // If we are here, we are at the topmost group to be visited in the recursion.
        // This is the group containing the control we will check against.
        double sales_target = 0;
        if (schedule[reportStepIdx].gconsale().has(group.name())) {
            const auto& gconsale = schedule[reportStepIdx].gconsale().get(group.name(), summaryState);
            sales_target = gconsale.sales_target;
        }
        InjectionTargetCalculator tcalc(currentGroupControl, pu, resv_coeff, group.name(), sales_target, wellState, injectionPhase, deferred_logger);
        FractionCalculator fcalc(schedule, summaryState, wellState, reportStepIdx, guideRate, tcalc.guideTargetMode(), pu, false, injectionPhase);

        auto localFraction = [&](const std::string& child) { return fcalc.localFraction(child, name); };

        auto localReduction = [&](const std::string& group_name) {
            const std::vector<double>& groupTargetReductions
                = wellState.currentInjectionGroupReductionRates(group_name);
            return tcalc.calcModeRateFromRates(groupTargetReductions);
        };

        const double orig_target = tcalc.groupTarget(group.injectionControls(injectionPhase, summaryState), deferred_logger);
        // Assume we have a chain of groups as follows: BOTTOM -> MIDDLE -> TOP.
        // Then ...
        // TODO finish explanation.
        const double current_rate
            = tcalc.calcModeRateFromRates(rates); // Switch sign since 'rates' are negative for producers.
        const auto chain = groupChainTopBot(name, group.name(), schedule, reportStepIdx);
        // Because 'name' is the last of the elements, and not an ancestor, we subtract one below.
        const size_t num_ancestors = chain.size() - 1;
        // we need to find out the level where the current well is applied to the local reduction
        size_t local_reduction_level = 0;
        for (size_t ii = 0; ii < num_ancestors; ++ii) {
            if ((ii == 0) || guideRate->has(chain[ii], injectionPhase)) {
                local_reduction_level = ii;
            }
        }

        double efficiencyFactorInclGroup = efficiencyFactor * group.getGroupEfficiencyFactor();
        double target = orig_target;
        for (size_t ii = 0; ii < num_ancestors; ++ii) {
            if ((ii == 0) || guideRate->has(chain[ii], injectionPhase)) {
                // Apply local reductions only at the control level
                // (top) and for levels where we have a specified
                // group guide rate.
                target -= localReduction(chain[ii]);

                // Add my reduction back at the level where it is included in the local reduction
                if (local_reduction_level == ii )
                    target += current_rate * efficiencyFactorInclGroup;
            }
            if (ii < num_ancestors - 1) {
                // Not final level. Add sub-level reduction back, if
                // it was nonzero due to having no group-controlled
                // wells.  Note that we make this call without setting
                // the current well to be always included, because we
                // want to know the situation that applied to the
                // calculation of reductions.
                const int num_gr_ctrl = groupControlledWells(schedule, wellState, reportStepIdx, chain[ii + 1], "", /*is_producer*/false, injectionPhase);
                if (num_gr_ctrl == 0) {
                    if (guideRate->has(chain[ii + 1], injectionPhase)) {
                        target += localReduction(chain[ii + 1]);
                    }
                }
            }
            target *= localFraction(chain[ii + 1]);
        }
        // Avoid negative target rates comming from too large local reductions.
        const double target_rate = std::max(1e-12, target / efficiencyFactorInclGroup);
        return std::make_pair(current_rate > target_rate, target_rate / current_rate);
    }



} // namespace WellGroupHelpers

} // namespace Opm
