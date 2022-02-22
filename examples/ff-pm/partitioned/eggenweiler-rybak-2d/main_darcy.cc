// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*****************************************************************************
 *   See the file COPYING for full copying permissions.                      *
 *                                                                           *
 *   This program is free software: you can redistribute it and/or modify    *
 *   it under the terms of the GNU General Public License as published by    *
 *   the Free Software Foundation, either version 3 of the License, or       *
 *   (at your option) any later version.                                     *
 *                                                                           *
 *   This program is distributed in the hope that it will be useful,         *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
 *   GNU General Public License for more details.                            *
 *                                                                           *
 *   You should have received a copy of the GNU General Public License       *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 *****************************************************************************/
/*!
 * \file
 * \ingroup BoundaryTests
 * \brief A test problem for the coupled FreeFlow/Darcy problem (1p).
 */

#include <config.h>

#include <iostream>

#include <dune/common/parallel/mpihelper.hh>
#include <dune/common/timer.hh>

#include <dumux/assembly/fvassembler.hh>
#include <dumux/common/dumuxmessage.hh>
#include <dumux/common/parameters.hh>
#include <dumux/common/partial.hh>
#include <dumux/common/properties.hh>
#include <dumux/io/grid/gridmanager_yasp.hh>
#include <dumux/io/staggeredvtkoutputmodule.hh>
#include <dumux/io/vtkoutputmodule.hh>
#include <dumux/linear/seqsolverbackend.hh>

#include <dumux/multidomain/fvassembler.hh>
#include <dumux/multidomain/newtonsolver.hh>
#include <dumux/multidomain/staggeredtraits.hh>

#include "properties.hh"

#include "problem_darcy.hh"

// #include <test/freeflow/navierstokes/analyticalsolutionvectors.hh>
// #include <test/freeflow/navierstokes/errors.hh>

#include "dumux-precice/couplingadapter.hh"

/*!
* \brief Creates analytical solution.
* Returns a tuple of the analytical solution for the pressure, the velocity and the velocity at the faces
* \param problem the problem for which to evaluate the analytical solution
*/
template<class Scalar, class Problem>
auto createDarcyAnalyticalSolution(const Problem &problem)
{
    const auto &gridGeometry = problem.gridGeometry();
    using GridView = typename std::decay_t<decltype(gridGeometry)>::GridView;

    static constexpr auto dim = GridView::dimension;
    static constexpr auto dimWorld = GridView::dimensionworld;

    using VelocityVector = Dune::FieldVector<Scalar, dimWorld>;

    std::vector<Scalar> analyticalPressure;
    std::vector<VelocityVector> analyticalVelocity;

    analyticalPressure.resize(gridGeometry.numDofs());
    analyticalVelocity.resize(gridGeometry.numDofs());
    auto fvGeometry = localView(gridGeometry);
    for (const auto &element : elements(gridGeometry.gridView())) {
        fvGeometry.bindElement(element);
        for (auto &&scv : scvs(fvGeometry)) {
            const auto ccDofIdx = scv.dofIndex();
            const auto ccDofPosition = scv.dofPosition();
            const auto analyticalSolutionAtCc =
                problem.analyticalSolution(ccDofPosition);
            analyticalPressure[ccDofIdx] = analyticalSolutionAtCc[dim];

            for (int dirIdx = 0; dirIdx < dim; ++dirIdx)
                analyticalVelocity[ccDofIdx][dirIdx] =
                    analyticalSolutionAtCc[dirIdx];
        }
    }

    return std::make_tuple(analyticalPressure, analyticalVelocity);
}

template<class Problem, class SolutionVector>
void printDarcyL2Error(const Problem &problem, const SolutionVector &x)
{
    using namespace Dumux;
    using Scalar = double;

    Scalar l2error = 0.0;
    auto fvGeometry = localView(problem.gridGeometry());
    for (const auto &element : elements(problem.gridGeometry().gridView())) {
        fvGeometry.bindElement(element);

        for (auto &&scv : scvs(fvGeometry)) {
            const auto dofIdx = scv.dofIndex();
            const Scalar delta =
                x[dofIdx] - problem.analyticalSolution(
                                scv.dofPosition())[2 /*pressureIdx*/];
            l2error += scv.volume() * (delta * delta);
        }
    }
    using std::sqrt;
    l2error = sqrt(l2error);

    const auto numDofs = problem.gridGeometry().numDofs();
    std::ostream tmp(std::cout.rdbuf());
    tmp << std::setprecision(8) << "** L2 error (abs) for " << std::setw(6)
        << numDofs << " cc dofs " << std::scientific << "L2 error = " << l2error
        << std::endl;

    // write the norm into a log file
    std::ofstream logFile;
    logFile.open(problem.name() + ".log", std::ios::app);
    logFile << "[ConvergenceTest] L2(p) = " << l2error << std::endl;
    logFile.close();
}

int main(int argc, char **argv)
{
    using namespace Dumux;

    // initialize MPI, finalize is done automatically on exit
    const auto &mpiHelper = Dune::MPIHelper::instance(argc, argv);

    // print dumux start message
    if (mpiHelper.rank() == 0)
        DumuxMessage::print(/*firstCall=*/true);

    // parse command line arguments and input file
    Parameters::init(argc, argv);

    // Define the sub problem type tags
    using DarcyTypeTag = Properties::TTag::DarcyOnePBox;

    // try to create a grid (from the given grid file or the input file)
    // for both sub-domains
    using DarcyGridManager =
        Dumux::GridManager<GetPropType<DarcyTypeTag, Properties::Grid>>;
    DarcyGridManager darcyGridManager;
    darcyGridManager.init("Darcy");  // pass parameter group

    // we compute on the leaf grid view
    const auto &darcyGridView = darcyGridManager.grid().leafGridView();

    // create the finite volume grid geometry
    using DarcyGridGeometry =
        GetPropType<DarcyTypeTag, Properties::GridGeometry>;
    auto darcyGridGeometry = std::make_shared<DarcyGridGeometry>(darcyGridView);

    // using Traits = StaggeredMultiDomainTraits<FreeFlowTypeTag, FreeFlowTypeTag,
    //                                           DarcyTypeTag>;
    //using Traits = StaggeredMultiDomainTraits<FreeFlowTypeTag, FreeFlowTypeTag,
    //                                           DarcyTypeTag>

    // the indices
    // constexpr auto freeFlowCellCenterIdx =
    //     CouplingManager::freeFlowCellCenterIdx;
    // constexpr auto freeFlowFaceIdx = CouplingManager::freeFlowFaceIdx;
    // constexpr auto porousMediumIdx = CouplingManager::porousMediumIdx;

    // the problem (initial and boundary conditions)
    // using FreeFlowProblem = GetPropType<FreeFlowTypeTag, Properties::Problem>;
    // auto freeFlowProblem = std::make_shared<FreeFlowProblem>(
    //     freeFlowGridGeometry, couplingManager);
    using DarcyProblem = GetPropType<DarcyTypeTag, Properties::Problem>;
    // auto spatialParams = std::make_shared<typename DarcyProblem::SpatialParams>(
    //     darcyGridGeometry);
    // auto darcyProblem = std::make_shared<DarcyProblem>(
    //     darcyGridGeometry, couplingManager, spatialParams);
    auto darcyProblem = std::make_shared<DarcyProblem>(darcyGridGeometry);

    // the solution vector
    // Traits::SolutionVector sol;
    GetPropType<DarcyTypeTag, Properties::SolutionVector> sol;
    sol.resize(darcyGridGeometry->numDofs());

    // Initialize preCICE.Tell preCICE about:
    // - Name of solver
    // - What rank of how many ranks this instance is
    // Configure preCICE. For now the config file is hardcoded.
    //couplingInterface.createInstance( "darcy", mpiHelper.rank(), mpiHelper.size() );
    // std::string preciceConfigFilename = ;
    //    if (argc == 3)
    //      preciceConfigFilename = argv[2];
    // if (argc > 2)
    const std::string preciceConfigFilename =
        (argc > 2) ? argv[argc - 1] : "precice-config.xml";

    auto &couplingInterface = Dumux::Precice::CouplingAdapter::getInstance();
    couplingInterface.announceSolver("Darcy", preciceConfigFilename,
                                     mpiHelper.rank(), mpiHelper.size());

    // get a solution vector storing references to the two FreeFlow solution vectors
    //auto freeFlowSol = partial(sol, freeFlowFaceIdx, freeFlowCellCenterIdx);

    // couplingManager->init(freeFlowProblem, darcyProblem, sol);

    // the grid variables
    using DarcyGridVariables =
        GetPropType<DarcyTypeTag, Properties::GridVariables>;
    auto darcyGridVariables =
        std::make_shared<DarcyGridVariables>(darcyProblem, darcyGridGeometry);
    darcyGridVariables->init(sol);

    // intialize the vtk output module
    //using Scalar = typename Traits::Scalar;
    using Scalar = GetPropType<DarcyTypeTag, Properties::Scalar>;
    // StaggeredVtkOutputModule<FreeFlowGridVariables, decltype(freeFlowSol)>
    //     freeFlowVtkWriter(*freeFlowGridVariables, freeFlowSol,
    //                       freeFlowProblem->name());
    // GetPropType<FreeFlowTypeTag, Properties::IOFields>::initOutputModule(
    //     freeFlowVtkWriter);

    // NavierStokesAnalyticalSolutionVectors freeFlowAnalyticalSolVectors(
    //     freeFlowProblem);
    // freeFlowVtkWriter.addField(
    //     freeFlowAnalyticalSolVectors.getAnalyticalPressureSolution(),
    //     "pressureExact");
    // freeFlowVtkWriter.addField(
    //     freeFlowAnalyticalSolVectors.getAnalyticalVelocitySolution(),
    //     "velocityExact");
    // freeFlowVtkWriter.addFaceField(
    //     freeFlowAnalyticalSolVectors.getAnalyticalVelocitySolutionOnFace(),
    //     "faceVelocityExact");

    // freeFlowVtkWriter.write(0.0);

    VtkOutputModule<DarcyGridVariables,
                    GetPropType<DarcyTypeTag, Properties::SolutionVector>>
        darcyVtkWriter(*darcyGridVariables, sol, darcyProblem->name());
    using DarcyVelocityOutput =
        GetPropType<DarcyTypeTag, Properties::VelocityOutput>;
    darcyVtkWriter.addVelocityOutput(
        std::make_shared<DarcyVelocityOutput>(*darcyGridVariables));
    GetPropType<DarcyTypeTag, Properties::IOFields>::initOutputModule(
        darcyVtkWriter);
    const auto darcyAnalyticalSolution =
        createDarcyAnalyticalSolution<Scalar>(*darcyProblem);
    darcyVtkWriter.addField(std::get<0>(darcyAnalyticalSolution),
                            "pressureExact");
    darcyVtkWriter.addField(std::get<1>(darcyAnalyticalSolution),
                            "velocityExact");
    darcyVtkWriter.write(0.0);

    // the assembler for a stationary problem
    // using Assembler =
    //     MultiDomainFVAssembler<Traits, CouplingManager, DiffMethod::numeric>;
    // auto assembler = std::make_shared<Assembler>(
    //     std::make_tuple(freeFlowProblem, freeFlowProblem, darcyProblem),
    //     std::make_tuple(freeFlowGridGeometry->faceFVGridGeometryPtr(),
    //                     freeFlowGridGeometry->cellCenterFVGridGeometryPtr(),
    //                     darcyGridGeometry),
    //     std::make_tuple(freeFlowGridVariables->faceGridVariablesPtr(),
    //                     freeFlowGridVariables->cellCenterGridVariablesPtr(),
    //                     darcyGridVariables),
    //     couplingManager);
    using Assembler = FVAssembler<DarcyTypeTag, DiffMethod::numeric>;
    auto assembler = std::make_shared<Assembler>(
        darcyProblem, darcyGridGeometry, darcyGridVariables);

    // the linear solver
    using LinearSolver = UMFPackBackend;
    auto linearSolver = std::make_shared<LinearSolver>();

    // the non-linear solver
    // using NewtonSolver =
    //     MultiDomainNewtonSolver<Assembler, LinearSolver, CouplingManager>;
    // NewtonSolver nonLinearSolver(assembler, linearSolver, couplingManager);
    using NewtonSolver = Dumux::NewtonSolver<Assembler, LinearSolver>;
    NewtonSolver nonLinearSolver(assembler, linearSolver);

    // solve the non-linear system
    nonLinearSolver.solve(sol);

    // write vtk output
    // freeFlowVtkWriter.write(1.0);
    darcyVtkWriter.write(1.0);

    // printFreeFlowL2Error(freeFlowProblem, freeFlowSol);
    printDarcyL2Error(*darcyProblem, sol);

    ////////////////////////////////////////////////////////////
    // finalize, print dumux message to say goodbye
    ////////////////////////////////////////////////////////////

    // print dumux end message
    if (mpiHelper.rank() == 0) {
        Parameters::print();
        DumuxMessage::print(/*firstCall=*/false);
    }

    return 0;
}  // end main
