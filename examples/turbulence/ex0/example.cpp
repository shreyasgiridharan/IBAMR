// ---------------------------------------------------------------------
//
// Copyright (c) 2017 - 2019 by the IBAMR developers
// All rights reserved.
//
// This file is part of IBAMR.
//
// IBAMR is free software and is distributed under the 3-clause BSD
// license. The full text of the license can be found in the file
// COPYRIGHT at the top level directory of IBAMR.
//
// ---------------------------------------------------------------------

// Config files
#include <IBAMR_config.h>
#include <IBTK_config.h>

#include <SAMRAI_config.h>

// Headers for basic PETSc functions
#include <petscsys.h>

// Headers for basic SAMRAI objects
#include <BergerRigoutsos.h>
#include <CartesianGridGeometry.h>
#include <LoadBalancer.h>
#include <StandardTagAndInitialize.h>

// Headers for application-specific algorithm/data structure objects
#include <ibamr/AdvDiffSemiImplicitHierarchyIntegrator.h>
#include <ibamr/INSVCStaggeredConservativeHierarchyIntegrator.h>
#include <ibamr/INSVCStaggeredHierarchyIntegrator.h>
#include <ibamr/INSVCStaggeredNonConservativeHierarchyIntegrator.h>
#include <ibamr/RelaxationLSMethod.h>
#include <ibamr/SurfaceTensionForceFunction.h>
#include <ibamr/TurbulenceSSTKOmegaSourceFunction.h>
#include <ibamr/TwoEquationTurbulenceHierarchyIntegrator.h>
#include <ibamr/app_namespaces.h>

#include <ibtk/AppInitializer.h>
#include <ibtk/IndexUtilities.h>
#include <ibtk/muParserCartGridFunction.h>
#include <ibtk/muParserRobinBcCoefs.h>

#include <numeric>

// Application
#include "SetFluidProperties.h"

void compute_velocity_profile(Pointer<PatchHierarchy<NDIM> > patch_hierarchy,
                              const int u_idx,
                              double lower_coordiantes[NDIM],
                              double upper_coordiantes[NDIM],
                              const double data_time,
                              const string& data_dump_dirname);

/*******************************************************************************
 * For each run, the input filename and restart information (if needed) must   *
 * be given on the command line.  For non-restarted case, command line is:     *
 *                                                                             *
 *    executable <input file name>                                             *
 *                                                                             *
 * For restarted run, command line is:                                         *
 *                                                                             *
 *    executable <input file name> <restart directory> <restart number>        *
 *                                                                             *
 *******************************************************************************/
int
main(int argc, char* argv[])
{
    // Initialize PETSc, MPI, and SAMRAI.
    PetscInitialize(&argc, &argv, NULL, NULL);
    SAMRAI_MPI::setCommunicator(PETSC_COMM_WORLD);
    SAMRAI_MPI::setCallAbortInSerialInsteadOfExit();
    SAMRAIManager::startup();

    // Increase maximum patch data component indices
    SAMRAIManager::setMaxNumberPatchDataEntries(2500);

    { // cleanup dynamically allocated objects prior to shutdown

        // Parse command line options, set some standard options from the input
        // file, initialize the restart database (if this is a restarted run),
        // and enable file logging.
        Pointer<AppInitializer> app_initializer = new AppInitializer(argc, argv, "turbulent_channel_flow.log");
        Pointer<Database> input_db = app_initializer->getInputDatabase();

        // Get various standard options set in the input file.
        const bool dump_viz_data = app_initializer->dumpVizData();
        const int viz_dump_interval = app_initializer->getVizDumpInterval();
        const bool uses_visit = dump_viz_data && !app_initializer->getVisItDataWriter().isNull();

        const bool dump_restart_data = app_initializer->dumpRestartData();
        const int restart_dump_interval = app_initializer->getRestartDumpInterval();
        const string restart_dump_dirname = app_initializer->getRestartDumpDirectory();

        const bool dump_postproc_data = app_initializer->dumpPostProcessingData();
        const int postproc_data_dump_interval = app_initializer->getPostProcessingDataDumpInterval();
        const string postproc_data_dump_dirname = app_initializer->getPostProcessingDataDumpDirectory();
        if (dump_postproc_data && (postproc_data_dump_interval > 0) && !postproc_data_dump_dirname.empty())
        {
            Utilities::recursiveMkdir(postproc_data_dump_dirname);
        }

        const bool dump_timer_data = app_initializer->dumpTimerData();
        const int timer_dump_interval = app_initializer->getTimerDumpInterval();

        // Create major algorithm and data objects that comprise the
        // application.  These objects are configured from the input database
        // and, if this is a restarted run, from the restart database.
        Pointer<INSVCStaggeredHierarchyIntegrator> time_integrator;
        const string discretization_form =
            app_initializer->getComponentDatabase("Main")->getString("discretization_form");
        const bool conservative_form = (discretization_form == "CONSERVATIVE");
        if (conservative_form)
        {
            time_integrator = new INSVCStaggeredConservativeHierarchyIntegrator(
                "INSVCStaggeredConservativeHierarchyIntegrator",
                app_initializer->getComponentDatabase("INSVCStaggeredConservativeHierarchyIntegrator"));
        }
        else if (!conservative_form)
        {
            time_integrator = new INSVCStaggeredNonConservativeHierarchyIntegrator(
                "INSVCStaggeredNonConservativeHierarchyIntegrator",
                app_initializer->getComponentDatabase("INSVCStaggeredNonConservativeHierarchyIntegrator"));
        }
        else
        {
            TBOX_ERROR("Unsupported solver type: " << discretization_form << "\n"
                                                   << "Valid options are: CONSERVATIVE, NON_CONSERVATIVE");
        }

        Pointer<TwoEquationTurbulenceHierarchyIntegrator> turb_hier_integrator =
            new TwoEquationTurbulenceHierarchyIntegrator(
                "TwoEquationTurbulenceHierarchyIntegrator",
                app_initializer->getComponentDatabase("TwoEquationTurbulenceHierarchyIntegrator"));
        time_integrator->registerAdvDiffHierarchyIntegrator(turb_hier_integrator);
        turb_hier_integrator->registerINSVCStaggeredHierarchyIntegrator(time_integrator);

        Pointer<CartesianGridGeometry<NDIM> > grid_geometry = new CartesianGridGeometry<NDIM>(
            "CartesianGeometry", app_initializer->getComponentDatabase("CartesianGeometry"));
        Pointer<PatchHierarchy<NDIM> > patch_hierarchy = new PatchHierarchy<NDIM>("PatchHierarchy", grid_geometry);

        Pointer<StandardTagAndInitialize<NDIM> > error_detector =
            new StandardTagAndInitialize<NDIM>("StandardTagAndInitialize",
                                               time_integrator,
                                               app_initializer->getComponentDatabase("StandardTagAndInitialize"));
        Pointer<BergerRigoutsos<NDIM> > box_generator = new BergerRigoutsos<NDIM>();
        Pointer<LoadBalancer<NDIM> > load_balancer =
            new LoadBalancer<NDIM>("LoadBalancer", app_initializer->getComponentDatabase("LoadBalancer"));
        Pointer<GriddingAlgorithm<NDIM> > gridding_algorithm =
            new GriddingAlgorithm<NDIM>("GriddingAlgorithm",
                                        app_initializer->getComponentDatabase("GriddingAlgorithm"),
                                        error_detector,
                                        box_generator,
                                        load_balancer);

        // register turbulent kinetic energy, w
        Pointer<CellVariable<NDIM, double> > k_var = new CellVariable<NDIM, double>("turbulent_kinetic_energy");
        turb_hier_integrator->registerKVariable(k_var);

        // register turbulent specific dissipation rate, w
        Pointer<CellVariable<NDIM, double> > w_var =
            new CellVariable<NDIM, double>("turbulent_specific_dissipation_rate");
        turb_hier_integrator->registerWVariable(w_var);

        // register advection velocity
        turb_hier_integrator->setAdvectionVelocityKEquation(k_var, time_integrator->getAdvectionVelocityVariable());
        turb_hier_integrator->setAdvectionVelocityWEquation(w_var, time_integrator->getAdvectionVelocityVariable());

        // Setup the INS maintained material properties.
        Pointer<Variable<NDIM> > rho_var;
        if (conservative_form)
        {
            rho_var = new SideVariable<NDIM, double>("rho");
        }
        else
        {
            rho_var = new CellVariable<NDIM, double>("rho");
        }
        time_integrator->registerMassDensityVariable(rho_var);

        Pointer<CellVariable<NDIM, double> > mu_var = new CellVariable<NDIM, double>("mu");
        time_integrator->registerViscosityVariable(mu_var);

        // turbulent viscosity
        Pointer<CellVariable<NDIM, double> > mu_t_var = new CellVariable<NDIM, double>("mu_t");
        time_integrator->registerTurbulentViscosityVariable(mu_t_var);

        // Array for input into callback function
        const double rho = input_db->getDouble("RHO");
        const double mu = input_db->getDouble("MU");

        // Callback functions can either be registered with the NS integrator, or the advection-diffusion integrator
        SetFluidProperties* ptr_SetFluidProperties = new SetFluidProperties("SetFluidProperties", rho, mu);
        time_integrator->registerResetFluidDensityFcn(&callSetFluidDensityCallbackFunction,
                                                      static_cast<void*>(ptr_SetFluidProperties));
        time_integrator->registerResetFluidViscosityFcn(&callSetFluidViscosityCallbackFunction,
                                                        static_cast<void*>(ptr_SetFluidProperties));
        // Create Eulerian initial condition specification objects.
        if (input_db->keyExists("VelocityInitialConditions"))
        {
            Pointer<CartGridFunction> u_init = new muParserCartGridFunction(
                "u_init", app_initializer->getComponentDatabase("VelocityInitialConditions"), grid_geometry);
            time_integrator->registerVelocityInitialConditions(u_init);
        }

        if (input_db->keyExists("PressureInitialConditions"))
        {
            Pointer<CartGridFunction> p_init = new muParserCartGridFunction(
                "p_init", app_initializer->getComponentDatabase("PressureInitialConditions"), grid_geometry);
            time_integrator->registerPressureInitialConditions(p_init);
        }
        if (input_db->keyExists("TurbulentViscosityInitialConditions"))
        {
            Pointer<CartGridFunction> mu_t_init = new muParserCartGridFunction(
                "mu_t_init",
                app_initializer->getComponentDatabase("TurbulentViscosityInitialConditions"),
                grid_geometry);
            time_integrator->registerTurbulentViscosityInitialConditions(mu_t_init);
        }
        if (input_db->keyExists("TurbulentKineticEnergyInitialConditions"))
        {
            Pointer<CartGridFunction> k_init = new muParserCartGridFunction(
                "k_init",
                app_initializer->getComponentDatabase("TurbulentKineticEnergyInitialConditions"),
                grid_geometry);
            turb_hier_integrator->setInitialConditionsKEquation(k_var, k_init);
        }
        if (input_db->keyExists("TurbulentSpecificDissipationRateInitialConditions"))
        {
            Pointer<CartGridFunction> w_init = new muParserCartGridFunction(
                "w_init",
                app_initializer->getComponentDatabase("TurbulentSpecificDissipationRateInitialConditions"),
                grid_geometry);
            turb_hier_integrator->setInitialConditionsWEquation(w_var, w_init);
        }
        // Create Eulerian boundary condition specification objects (when necessary).
        const IntVector<NDIM>& periodic_shift = grid_geometry->getPeriodicShift();
        vector<RobinBcCoefStrategy<NDIM>*> u_bc_coefs(NDIM);
        if (periodic_shift.min() > 0)
        {
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                u_bc_coefs[d] = NULL;
            }
        }
        else
        {
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                const std::string bc_coefs_name = "u_bc_coefs_" + std::to_string(d);

                const std::string bc_coefs_db_name = "VelocityBcCoefs_" + std::to_string(d);

                u_bc_coefs[d] = new muParserRobinBcCoefs(
                    bc_coefs_name, app_initializer->getComponentDatabase(bc_coefs_db_name), grid_geometry);
            }
            time_integrator->registerPhysicalBoundaryConditions(u_bc_coefs);
        }

        RobinBcCoefStrategy<NDIM>* rho_bc_coef = NULL;
        if (!(periodic_shift.min() > 0) && input_db->keyExists("RhoBcCoefs"))
        {
            rho_bc_coef = new muParserRobinBcCoefs(
                "rho_bc_coef", app_initializer->getComponentDatabase("RhoBcCoefs"), grid_geometry);
            time_integrator->registerMassDensityBoundaryConditions(rho_bc_coef);
        }

        RobinBcCoefStrategy<NDIM>* mu_bc_coef = NULL;
        if (!(periodic_shift.min() > 0) && input_db->keyExists("MuBcCoefs"))
        {
            mu_bc_coef = new muParserRobinBcCoefs(
                "mu_bc_coef", app_initializer->getComponentDatabase("MuBcCoefs"), grid_geometry);
            time_integrator->registerViscosityBoundaryConditions(mu_bc_coef);
        }

        RobinBcCoefStrategy<NDIM>* mu_t_bc_coef = NULL;
        if (!(periodic_shift.min() > 0) && input_db->keyExists("MutBcCoefs"))
        {
            mu_t_bc_coef = new muParserRobinBcCoefs(
                "mu_t_bc_coef", app_initializer->getComponentDatabase("MutBcCoefs"), grid_geometry);
            time_integrator->registerTurbulentViscosityBoundaryConditions(mu_t_bc_coef);
        }
        RobinBcCoefStrategy<NDIM>* k_bc_coef = NULL;
        if (!(periodic_shift.min() > 0) && input_db->keyExists("KBcCoefs"))
        {
            k_bc_coef =
                new muParserRobinBcCoefs("k_bc_coef", app_initializer->getComponentDatabase("KBcCoefs"), grid_geometry);
            turb_hier_integrator->setPhysicalBcCoefKEquation(k_var, k_bc_coef);
        }

        RobinBcCoefStrategy<NDIM>* w_bc_coef = NULL;
        if (!(periodic_shift.min() > 0) && input_db->keyExists("WBcCoefs"))
        {
            w_bc_coef =
                new muParserRobinBcCoefs("w_bc_coef", app_initializer->getComponentDatabase("WBcCoefs"), grid_geometry);
            turb_hier_integrator->setPhysicalBcCoefWEquation(w_var, w_bc_coef);
        }

        // set source terms
        Pointer<TurbulenceSSTKOmegaSourceFunction> F_fcn = new TurbulenceSSTKOmegaSourceFunction(
            "TurbulenceSSTKOmegaSourceFunction",
            app_initializer->getComponentDatabase("TurbulenceSSTKOmegaSourceFunction"),
            turb_hier_integrator,
            time_integrator);
        turb_hier_integrator->setSourceTermFunctionKEquation(k_var, F_fcn);
        turb_hier_integrator->setSourceTermFunctionWEquation(w_var, F_fcn);

        // Set up visualization plot file writers.
        Pointer<VisItDataWriter<NDIM> > visit_data_writer = app_initializer->getVisItDataWriter();
        if (uses_visit)
        {
            time_integrator->registerVisItDataWriter(visit_data_writer);
        }

        // Initialize hierarchy configuration and data on all patches.
        time_integrator->initializePatchHierarchy(patch_hierarchy, gridding_algorithm);

        // Remove the AppInitializer
        app_initializer.setNull();

        // Print the input database contents to the log file.
        plog << "Input database:\n";
        input_db->printClassData(plog);

        // Write out initial visualization data.
        int iteration_num = time_integrator->getIntegratorStep();
        double loop_time = time_integrator->getIntegratorTime();
        if (dump_viz_data && uses_visit)
        {
            pout << "\n\nWriting visualization files...\n\n";
            time_integrator->setupPlotData();
            visit_data_writer->writePlotData(patch_hierarchy, iteration_num, loop_time);
        }

        // Main time step loop.
        double loop_time_end = time_integrator->getEndTime();
        double dt = 0.0;
        while (!MathUtilities<double>::equalEps(loop_time, loop_time_end) && time_integrator->stepsRemaining())
        {
            iteration_num = time_integrator->getIntegratorStep();
            loop_time = time_integrator->getIntegratorTime();

            pout << "\n";
            pout << "+++++++++++++++++++++++++++++++++++++++++++++++++++\n";
            pout << "At beginning of timestep # " << iteration_num << "\n";
            pout << "Simulation time is " << loop_time << "\n";

            dt = time_integrator->getMaximumTimeStepSize();
            time_integrator->advanceHierarchy(dt);
            loop_time += dt;

            pout << "\n";
            pout << "At end       of timestep # " << iteration_num << "\n";
            pout << "Simulation time is " << loop_time << "\n";
            pout << "+++++++++++++++++++++++++++++++++++++++++++++++++++\n";
            pout << "\n";

            // At specified intervals, write visualization and restart files,
            // print out timer data, and store hierarchy data for post
            // processing.
            iteration_num += 1;
            const bool last_step = !time_integrator->stepsRemaining();
            if (dump_viz_data && uses_visit && (iteration_num % viz_dump_interval == 0 || last_step))
            {
                pout << "\nWriting visualization files...\n\n";
                time_integrator->setupPlotData();
                visit_data_writer->writePlotData(patch_hierarchy, iteration_num, loop_time);
            }
            if (dump_postproc_data && (iteration_num % postproc_data_dump_interval == 0 || last_step))
            {
                Pointer<SideVariable<NDIM, double> > u_var = time_integrator->getVelocityVariable();
                VariableDatabase<NDIM>* var_db = VariableDatabase<NDIM>::getDatabase();
                const int u_idx = var_db->mapVariableAndContextToIndex(u_var, time_integrator->getCurrentContext());
                double lower_coordinates[NDIM], upper_coordinates[NDIM];
                if (input_db->keyExists("output_velocity_profile"))
                {
                    Pointer<Database> db = input_db->getDatabase("output_velocity_profile");
                    db->getDoubleArray("lower_coordinates", lower_coordinates, NDIM);
                    db->getDoubleArray("upper_coordinates", upper_coordinates, NDIM);
                }
                compute_velocity_profile(patch_hierarchy,
                                         u_idx,
                                         lower_coordinates,
                                         upper_coordinates,
                                         loop_time,
                                         postproc_data_dump_dirname);
            }
        }

        // Cleanup Eulerian boundary condition specification objects (when
        // necessary).
        for (unsigned int d = 0; d < NDIM; ++d) delete u_bc_coefs[d];
        // Cleanup other dumb pointers
        delete ptr_SetFluidProperties;

    } // cleanup dynamically allocated objects prior to shutdown

    SAMRAIManager::shutdown();
    PetscFinalize();
} // main

void
compute_velocity_profile(Pointer<PatchHierarchy<NDIM> > patch_hierarchy,
                         const int u_idx,
                         double lower_coordinates[NDIM],
                         double upper_coordinates[NDIM],
                         const double data_time,
                         const string& data_dump_dirname)
{
    const int coarsest_ln = 0;
    const int finest_ln = patch_hierarchy->getFinestLevelNumber();
    const double x_loc = lower_coordinates[0];
    const double y_loc_min = lower_coordinates[1];
    const double y_loc_max = upper_coordinates[1];
    const double X_min[2] = { x_loc, y_loc_min };
    const double X_max[2] = { x_loc, y_loc_max };
    vector<double> pos_values;
    for (int ln = finest_ln; ln >= coarsest_ln; --ln)
    {
        Pointer<PatchLevel<NDIM> > level = patch_hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            const Box<NDIM>& patch_box = patch->getBox();
            const CellIndex<NDIM>& patch_lower = patch_box.lower();
            const CellIndex<NDIM>& patch_upper = patch_box.upper();
            const Pointer<CartesianPatchGeometry<NDIM> > patch_geom = patch->getPatchGeometry();
            const double* const patch_x_lower = patch_geom->getXLower();
            const double* const patch_x_upper = patch_geom->getXUpper();
            const double* const patch_dx = patch_geom->getDx();
            const bool inside_patch = x_loc >= patch_x_lower[0] && x_loc <= patch_x_upper[0] &&
                                      !(patch_x_upper[1] < y_loc_min || patch_x_lower[1] > y_loc_max);
            if (!inside_patch) continue;
            // Entire box containing the required data.
            Box<NDIM> box(IndexUtilities::getCellIndex(
                              &X_min[0], patch_x_lower, patch_x_upper, patch_dx, patch_lower, patch_upper),
                          IndexUtilities::getCellIndex(
                              &X_max[0], patch_x_lower, patch_x_upper, patch_dx, patch_lower, patch_upper));
            // Part of the box on this patch
            Box<NDIM> trim_box = patch_box * box;
            BoxList<NDIM> iterate_box_list = trim_box;
            // Trim the box covered by the finer region
            BoxList<NDIM> covered_boxes;
            if (ln < finest_ln)
            {
                BoxArray<NDIM> refined_region_boxes;
                Pointer<PatchLevel<NDIM> > next_finer_level = patch_hierarchy->getPatchLevel(ln + 1);
                refined_region_boxes = next_finer_level->getBoxes();
                refined_region_boxes.coarsen(next_finer_level->getRatioToCoarserLevel());
                for (int i = 0; i < refined_region_boxes.getNumberOfBoxes(); ++i)
                {
                    const Box<NDIM> refined_box = refined_region_boxes[i];
                    const Box<NDIM> covered_box = trim_box * refined_box;
                    covered_boxes.unionBoxes(covered_box);
                }
            }
            iterate_box_list.removeIntersections(covered_boxes);
            // Loop over the boxes and store the location and interpolated value.
            Pointer<SideData<NDIM, double> > u_data = patch->getPatchData(u_idx);
            for (BoxList<NDIM>::Iterator lit(iterate_box_list); lit; lit++)
            {
                const Box<NDIM>& iterate_box = *lit;
                for (Box<NDIM>::Iterator bit(iterate_box); bit; bit++)
                {
                    const CellIndex<NDIM>& lower_idx = *bit;
                    CellIndex<NDIM> upper_idx = lower_idx;
                    upper_idx(0) += 1;
                    const double y = patch_x_lower[1] + patch_dx[1] * (lower_idx(1) - patch_lower(1) + 0.5);
                    const double x0 = patch_x_lower[0] + patch_dx[0] * (lower_idx(0) - patch_lower(0));
                    const double x1 = x0 + patch_dx[0];
                    const double u0 = (*u_data)(SideIndex<NDIM>(lower_idx, 0, SideIndex<NDIM>::Lower));
                    const double u1 = (*u_data)(SideIndex<NDIM>(upper_idx, 0, SideIndex<NDIM>::Lower));
                    pos_values.push_back(y);
                    pos_values.push_back(u0 + (u1 - u0) * (x_loc - x0) / (x1 - x0));
                }
            }
        }
    }
    const int nprocs = SAMRAI_MPI::getNodes();
    const int rank = SAMRAI_MPI::getRank();
    vector<int> data_size(nprocs, 0);
    data_size[rank] = static_cast<int>(pos_values.size());
    SAMRAI_MPI::sumReduction(&data_size[0], nprocs);
    int offset = 0;
    offset = std::accumulate(&data_size[0], &data_size[rank], offset);
    int size_array = 0;
    size_array = std::accumulate(&data_size[0], &data_size[0] + nprocs, size_array);
    // Write out the result in a file.
    string file_name = data_dump_dirname + "/" + "u_y_";
    char temp_buf[128];
    sprintf(temp_buf, "%.8f", data_time);
    file_name += temp_buf;
    MPI_Status status;
    MPI_Offset mpi_offset;
    MPI_File file;
    MPI_File_open(MPI_COMM_WORLD, file_name.c_str(), MPI_MODE_CREATE | MPI_MODE_WRONLY, MPI_INFO_NULL, &file);
    // First write the total size of the array.
    if (rank == 0)
    {
        mpi_offset = 0;
        MPI_File_seek(file, mpi_offset, MPI_SEEK_SET);
        MPI_File_write(file, &size_array, 1, MPI_INT, &status);
    }
    mpi_offset = sizeof(double) * offset + sizeof(int);
    MPI_File_seek(file, mpi_offset, MPI_SEEK_SET);
    MPI_File_write(file, &pos_values[0], data_size[rank], MPI_DOUBLE, &status);
    MPI_File_close(&file);
    return;
} // compute_velocity_profile