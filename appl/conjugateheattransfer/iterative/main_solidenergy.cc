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
 * \brief Main file for the heat problem.
 */

#include <config.h>

#include <ctime>
#include <iostream>

#include <dune/common/parallel/mpihelper.hh>
#include <dune/common/timer.hh>
#include <dune/istl/io.hh>

#include <dumux/common/properties.hh>
#include <dumux/common/parameters.hh>
#include <dumux/common/partial.hh>
#include <dumux/common/dumuxmessage.hh>
#include <dumux/linear/seqsolverbackend.hh>
#include <dumux/assembly/fvassembler.hh>
#include <dumux/assembly/diffmethod.hh>
#include <dumux/discretization/method.hh>
#include <dumux/io/vtkoutputmodule.hh>
#include <dumux/io/grid/gridmanager.hh>

#include <dumux/nonlinear/newtonsolver.hh>

#include <precice/SolverInterface.hpp>

#include "../monolithic/problem_heat.hh"

int main(int argc, char** argv) try
{
    using namespace Dumux;

    // initialize MPI, finalize is done automatically on exit
    const auto& mpiHelper = Dune::MPIHelper::instance(argc, argv);

    // print dumux start message
    if (mpiHelper.rank() == 0)
        DumuxMessage::print(/*firstCall=*/true);

    // parse command line arguments and input file
    Parameters::init(argc, argv);

    // Define the sub problem type tag
    using SolidEnergyTypeTag = Properties::TTag::HeatModel;

    // try to create a grid (from the given grid file or the input file)
    // for both sub-domains
    using SolidEnergyGridManager = Dumux::GridManager<GetPropType<SolidEnergyTypeTag, Properties::Grid>>;
    SolidEnergyGridManager solidEnergyGridManager;
    solidEnergyGridManager.init("SolidEnergy"); // pass parameter group

    // we compute on the leaf grid view
    const auto& solidEnergyGridView = solidEnergyGridManager.grid().leafGridView();

    using SolidEnergyFVGridGeometry = GetPropType<SolidEnergyTypeTag, Properties::FVGridGeometry>;
    auto solidEnergyFvGridGeometry = std::make_shared<SolidEnergyFVGridGeometry>(solidEnergyGridView);
    solidEnergyFvGridGeometry->update();

    // the problem (initial and boundary conditions)
    using SolidEnergyProblem = GetPropType<SolidEnergyTypeTag, Properties::Problem>;
    auto solidEnergyProblem = std::make_shared<SolidEnergyProblem>(solidEnergyFvGridGeometry);

    // the solution vector
    GetPropType<SolidEnergyTypeTag, Properties::SolutionVector> sol;
    sol.resize(solidEnergyFvGridGeometry->numDofs());

    // Initialize preCICE.Tell preCICE about:
    // - Name of solver
    // - What rank of how many ranks this instance is
    precice::SolverInterface precice("SolidEnergy", mpiHelper.rank(), mpiHelper.size() );

    // Configure preCICE. For now the config file is hardcoded.
    precice.configure("precice-config.xml");

    // Constants for checkpointing
    const std::string& readCheckpoint = precice::constants::actionReadIterationCheckpoint(); 
    const std::string& writeCheckpoint = precice::constants::actionWriteIterationCheckpoint();

    const int dim = precice.getDimensions();
    if (dim != int(FreeFlowFVGridGeometry::GridView::dimension))
        DUNE_THROW(Dune::InvalidStateException, "Dimensions do not match");
    const int meshId = precice.getMeshID("SolidEnergyMesh");

    std::vector<double> coords; //( dim * vertexSize );
    std::vector<std::size_t> coupledScvfIndices;

    for (const auto& element : elements(freeFlowGridView))
    {
        auto fvGeometry = localView(*freeFlowFvGridGeometry);
        fvGeometry.bindElement(element);

        for (const auto& scvf : scvfs(fvGeometry))
        {
            static constexpr auto eps = 1e-7;
            const auto& pos = scvf.center();
            if (pos[1] > freeFlowFvGridGeometry->bBoxx()[1] - eps)
            {
                coupledScvfIndices.push_back(scvf.index());
                for (const auto p : pos)
                    coords.push_back(p);
            }
        }
    }

    const auto vertexSize = coords.size() / dim;

    std::vector<int> vertexIds( vertexSize );
    precice.setMeshVertices( meshId, vertexSize, coords.data(), vertexIds.data() );

    const int temperatureId = precice.getDataID( "Temperature", meshId );
    const int heatFluxId = precice.getDataID( "Heat-Flux", meshId );

    // TODO
    /* 
    std::vector<double> temperatureVec( vertexSize );
    std::vector<double> heatFluxVec( vertexSize );
    */

    // apply initial solution for instationary problems
    solidEnergyProblem->applyInitialSolution(sol);

    //TODO: If necessary, communicate initial data to other solver
    /*
    if ( precice.isActionRequired(precice::constants::actionWriteInitialData()) )
    {
      //Fill data vector
      for (int i = 0; i < vertexSize; ++i)
      {
        //temperatureVec[i] = ??; 
      }
       precice.writeBlockScalarData( heatFluxId, vertexSize, vertexIDs.data(), heatFluxVec.data() );
       precice.fulfilledAction(precice::constants::actionWriteInitialData());
    }
    */

    const double preciceDt = precice.initialize();
    precice.initializeData();

    // Read initialdata for heat-flux if available
    /*
    if (precice.isReadDataAvailable())
    {
      precice.readBlockScalarData( temperatureId, vertexSize, vertexIDs.data(), temperatureVec.data() );
    }
    */

    auto solOld = sol;

    // the grid variables
    using SolidEnergyGridVariables = GetPropType<SolidEnergyTypeTag, Properties::GridVariables>;
    auto solidEnergyGridVariables = std::make_shared<SolidEnergyGridVariables>(solidEnergyProblem, solidEnergyFvGridGeometry);
    solidEnergyGridVariables->init(sol);

    // intialize the vtk output module
    VtkOutputModule<SolidEnergyGridVariables, GetPropType<SolidEnergyTypeTag, Properties::SolutionVector>> solidEnergyVtkWriter(*solidEnergyGridVariables, sol,  solidEnergyProblem->name());
    GetPropType<SolidEnergyTypeTag, Properties::IOFields>::initOutputModule(solidEnergyVtkWriter);
    solidEnergyVtkWriter.write(0.0);

    // instantiate time loop
    using Scalar = GetPropType<SolidEnergyTypeTag, Properties::Scalar>;
    const auto tEnd = getParam<Scalar>("TimeLoop.TEnd");
    const auto maxDt = getParam<Scalar>("TimeLoop.MaxTimeStepSize");
    auto dt = getParam<Scalar>("TimeLoop.DtInitial");

    //Time step size can also be changed by preCICE
    dt = std::max( dt, preciceDt );

    auto timeLoop = std::make_shared<TimeLoop<Scalar>>(0, dt, tEnd);
    timeLoop->setMaxTimeStepSize(maxDt);

    // the assembler for a stationary problem
    using Assembler = FVAssembler<SolidEnergyTypeTag, DiffMethod::numeric>;
    auto assembler = std::make_shared<Assembler>(solidEnergyProblem, solidEnergyFvGridGeometry, solidEnergyGridVariables, timeLoop);

    // the linear solver
    using LinearSolver = UMFPackBackend;
    auto linearSolver = std::make_shared<LinearSolver>();

    // the non-linear solver
    using NewtonSolver = NewtonSolver<Assembler, LinearSolver>;
    NewtonSolver nonLinearSolver(assembler, linearSolver);

    //Checkpointing variable for preCICE
    auto sol_checkpoint = sol;

    // time loop
    timeLoop->start(); do
    {
        if ( precice.isActionRequired( writeCheckpoint ) )
        {
            //DO CHECKPOINTING
            sol_checkpoint = sol;
            precice.fulfilledAction( writeCheckpoint );
        }
        // Read heat flux from precice
        // TODO: Remove // when vertexSize is defined
        // precice.readBlockScalarData( heatFluxId, vertexSize, vertexIDs.data(), heatFluxVec.data() );

        // Make use of the heat flux here
        // TODO

        // set previous solution for storage evaluations
        assembler->setPreviousSolution(solOld);

        // solve the non-linear system with time step control
        nonLinearSolver.solve(sol, *timeLoop);

        // make the new solution the old solution

        //TODO DO WE HAVE TO MOVE THAT?
        solOld = sol;
        solidEnergyGridVariables->advanceTimeStep();

        if ( precice.isActionRequired( readCheckpoint ) )
        {
            //Read checkpoint
            sol = sol_checkpoint;
            freeFlowGridVariables->update(sol);
            precice.fulfilledAction( writeCheckpoint );
        }
        else // coupling successful
        {
            // advance to the time loop to the next step
            timeLoop->advanceTimeStep();

            // write vtk output
            solidEnergyVtkWriter.write(timeLoop->time());

            // report statistics of this time step
            timeLoop->reportTimeStep();

            // Get the temperature here
            // TODO

            // Write temperature to preCICe
            // TODO: Remove // when vertexSize is defined
            //precice.writeBlockScalarData( temperatureId, vertexSize, vertexIDs.data(), temperatureVec.data() );

            // set new dt as suggested by newton solver
            const double preciceDt = precice.advance( timeLoop->timeStepSize() );
            const double newDt = std::max( preciceDt, timeLoop->timeStepSize() );

            timeLoop->setTimeStepSize(nonLinearSolver.suggestTimeStepSize( newDt ));
        }

    } while (!timeLoop->finished() && precice.isCouplingOngoing());

    timeLoop->finalize(solidEnergyGridView.comm());

    ////////////////////////////////////////////////////////////
    // finalize, print dumux message to say goodbye
    ////////////////////////////////////////////////////////////

    // print dumux end message
    if (mpiHelper.rank() == 0)
    {
        Parameters::print();
        DumuxMessage::print(/*firstCall=*/false);
    }

    precice.finalize();

    return 0;
} // end main
catch (Dumux::ParameterException &e)
{
    std::cerr << std::endl << e << " ---> Abort!" << std::endl;
    return 1;
}
catch (Dune::DGFException & e)
{
    std::cerr << "DGF exception thrown (" << e <<
                 "). Most likely, the DGF file name is wrong "
                 "or the DGF file is corrupted, "
                 "e.g. missing hash at end of file or wrong number (dimensions) of entries."
                 << " ---> Abort!" << std::endl;
    return 2;
}
catch (Dune::Exception &e)
{
    std::cerr << "Dune reported error: " << e << " ---> Abort!" << std::endl;
    return 3;
}
catch (...)
{
    std::cerr << "Unknown exception thrown! ---> Abort!" << std::endl;
    return 4;
}
