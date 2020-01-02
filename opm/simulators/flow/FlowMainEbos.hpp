/*
  Copyright 2013, 2014, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2015 IRIS AS
  Copyright 2014 STATOIL ASA.

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
#ifndef OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED
#define OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED


#include <sys/utsname.h>

#include <opm/simulators/flow/BlackoilModelEbos.hpp>
#include <opm/simulators/flow/MissingFeatures.hpp>
#include <opm/simulators/flow/SimulatorFullyImplicitBlackoilEbos.hpp>
#include <opm/simulators/utils/ParallelFileMerger.hpp>
#include <opm/simulators/utils/moduleVersion.hpp>
#include <opm/simulators/linalg/ExtractParallelGridInformationToISTL.hpp>

#include <opm/core/props/satfunc/RelpermDiagnostics.hpp>

#include <opm/parser/eclipse/Deck/Deck.hpp>
#include <opm/parser/eclipse/Parser/Parser.hpp>
#include <opm/parser/eclipse/Parser/ParseContext.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/IOConfig/IOConfig.hpp>
#include <opm/parser/eclipse/EclipseState/InitConfig/InitConfig.hpp>
#include <opm/parser/eclipse/EclipseState/checkDeck.hpp>

#if HAVE_DUNE_FEM
#include <dune/fem/misc/mpimanager.hh>
#else
#include <dune/common/parallel/mpihelper.hh>
#endif

BEGIN_PROPERTIES

NEW_PROP_TAG(EnableDryRun);
NEW_PROP_TAG(OutputInterval);
NEW_PROP_TAG(UseAmg);
NEW_PROP_TAG(EnableLoggingFalloutWarning);

// TODO: enumeration parameters. we use strings for now.
SET_STRING_PROP(EclFlowProblem, EnableDryRun, "auto");
// Do not merge parallel output files or warn about them
SET_BOOL_PROP(EclFlowProblem, EnableLoggingFalloutWarning, false);
SET_INT_PROP(EclFlowProblem, OutputInterval, 1);

END_PROPERTIES

namespace Opm
{
    // The FlowMain class is the ebos based black-oil simulator.
    template <class TypeTag>
    class FlowMainEbos
    {
    public:
        typedef typename GET_PROP(TypeTag, MaterialLaw)::EclMaterialLawManager MaterialLawManager;
        typedef typename GET_PROP_TYPE(TypeTag, Simulator) EbosSimulator;
        typedef typename GET_PROP_TYPE(TypeTag, Grid) Grid;
        typedef typename GET_PROP_TYPE(TypeTag, GridView) GridView;
        typedef typename GET_PROP_TYPE(TypeTag, Problem) Problem;
        typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
        typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;

        typedef Opm::SimulatorFullyImplicitBlackoilEbos<TypeTag> Simulator;

        // Read the command line parameters. Throws an exception if something goes wrong.
        static int setupParameters_(int argc, char** argv)
        {
            // register the flow specific parameters
            EWOMS_REGISTER_PARAM(TypeTag, std::string, EnableDryRun,
                                 "Specify if the simulation ought to be actually run, or just pretended to be");
            EWOMS_REGISTER_PARAM(TypeTag, int, OutputInterval,
                                 "Specify the number of report steps between two consecutive writes of restart data");
            EWOMS_REGISTER_PARAM(TypeTag, bool, EnableLoggingFalloutWarning,
                                 "Developer option to see whether logging was on non-root processors. In that case it will be appended to the *.DBG or *.PRT files");

            Simulator::registerParameters();

            // register the parameters inherited from ebos
            Opm::registerAllParameters_<TypeTag>(/*finalizeRegistration=*/false);

            // hide the parameters unused by flow. TODO: this is a pain to maintain
            EWOMS_HIDE_PARAM(TypeTag, EnableGravity);
            EWOMS_HIDE_PARAM(TypeTag, EnableGridAdaptation);

            // this parameter is actually used in eWoms, but the flow well model
            // hard-codes the assumption that the intensive quantities cache is enabled,
            // so flow crashes. Let's hide the parameter for that reason.
            EWOMS_HIDE_PARAM(TypeTag, EnableIntensiveQuantityCache);

            // thermodynamic hints are not implemented/required by the eWoms blackoil
            // model
            EWOMS_HIDE_PARAM(TypeTag, EnableThermodynamicHints);

            // in flow only the deck file determines the end time of the simulation
            EWOMS_HIDE_PARAM(TypeTag, EndTime);

            // time stepping is not done by the eWoms code in flow
            EWOMS_HIDE_PARAM(TypeTag, InitialTimeStepSize);
            EWOMS_HIDE_PARAM(TypeTag, MaxTimeStepDivisions);
            EWOMS_HIDE_PARAM(TypeTag, MaxTimeStepSize);
            EWOMS_HIDE_PARAM(TypeTag, MinTimeStepSize);
            EWOMS_HIDE_PARAM(TypeTag, PredeterminedTimeStepsFile);

            EWOMS_HIDE_PARAM(TypeTag, EclMaxTimeStepSizeAfterWellEvent);
            EWOMS_HIDE_PARAM(TypeTag, EclRestartShrinkFactor);
            EWOMS_HIDE_PARAM(TypeTag, EclEnableTuning);

            // flow also does not use the eWoms Newton method
            EWOMS_HIDE_PARAM(TypeTag, NewtonMaxError);
            EWOMS_HIDE_PARAM(TypeTag, NewtonMaxIterations);
            EWOMS_HIDE_PARAM(TypeTag, NewtonTolerance);
            EWOMS_HIDE_PARAM(TypeTag, NewtonTargetIterations);
            EWOMS_HIDE_PARAM(TypeTag, NewtonVerbose);
            EWOMS_HIDE_PARAM(TypeTag, NewtonWriteConvergence);
            EWOMS_HIDE_PARAM(TypeTag, EclNewtonSumTolerance);
            EWOMS_HIDE_PARAM(TypeTag, EclNewtonSumToleranceExponent);
            EWOMS_HIDE_PARAM(TypeTag, EclNewtonStrictIterations);
            EWOMS_HIDE_PARAM(TypeTag, EclNewtonRelaxedVolumeFraction);
            EWOMS_HIDE_PARAM(TypeTag, EclNewtonRelaxedTolerance);

            // the default eWoms checkpoint/restart mechanism does not work with flow
            EWOMS_HIDE_PARAM(TypeTag, RestartTime);
            EWOMS_HIDE_PARAM(TypeTag, RestartWritingInterval);

            EWOMS_END_PARAM_REGISTRATION(TypeTag);

            int mpiRank = 0;
#if HAVE_MPI
            MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
#endif

            // read in the command line parameters
            int status = Opm::setupParameters_<TypeTag>(argc, const_cast<const char**>(argv), /*doRegistration=*/false, /*allowUnused=*/true, /*handleHelp=*/(mpiRank==0));
            if (status == 0) {

                // deal with unknown parameters.

                int unknownKeyWords = 0;
                if (mpiRank == 0) {
                    unknownKeyWords = Opm::Parameters::printUnused<TypeTag>(std::cerr);
                }
#if HAVE_MPI
                int globalUnknownKeyWords;
                MPI_Allreduce(&unknownKeyWords,  &globalUnknownKeyWords, 1, MPI_INT,  MPI_SUM, MPI_COMM_WORLD);
                unknownKeyWords = globalUnknownKeyWords;
#endif
                if ( unknownKeyWords )
                {
                    if ( mpiRank == 0 )
                    {
                        std::string msg = "Aborting simulation due to unknown "
                            "parameters. Please query \"flow --help\" for "
                            "supported command line parameters.";
                        if (OpmLog::hasBackend("STREAMLOG"))
                        {
                            OpmLog::error(msg);
                        }
                        else {
                            std::cerr << msg << std::endl;
                        }
                    }
                    return EXIT_FAILURE;
                }

                // deal with --print-properties and --print-parameters and unknown parameters.

                bool doExit = false;

                if (EWOMS_GET_PARAM(TypeTag, int, PrintProperties) == 1) {
                    doExit = true;
                    if (mpiRank == 0)
                        Opm::Properties::printValues<TypeTag>();
                }

                if (EWOMS_GET_PARAM(TypeTag, int, PrintParameters) == 1) {
                    doExit = true;
                    if (mpiRank == 0)
                        Opm::Parameters::printValues<TypeTag>();
                }

                if (doExit)
                    return -1;
            }

            return status;
        }

        static void printBanner()
        {
            const int lineLen = 70;
            const std::string version = moduleVersionName();
            const std::string banner = "This is flow "+version;
            const int bannerPreLen = (lineLen - 2 - banner.size())/2;
            const int bannerPostLen = bannerPreLen + (lineLen - 2 - banner.size())%2;
            std::cout << "**********************************************************************\n";
            std::cout << "*                                                                    *\n";
            std::cout << "*" << std::string(bannerPreLen, ' ') << banner << std::string(bannerPostLen, ' ') << "*\n";
            std::cout << "*                                                                    *\n";
            std::cout << "* Flow is a simulator for fully implicit three-phase black-oil flow, *\n";
            std::cout << "*             including solvent and polymer capabilities.            *\n";
            std::cout << "*          For more information, see https://opm-project.org         *\n";
            std::cout << "*                                                                    *\n";
            std::cout << "**********************************************************************\n\n";

            int threads = 1;
            int mpiSize = 1;

#ifdef _OPENMP
            // This function is called before the parallel OpenMP stuff gets initialized.
            // That initialization happends after the deck is read and we want this message.
            // Hence we duplicate the code of setupParallelism to get the number of threads.
            if (getenv("OMP_NUM_THREADS"))
                threads =  omp_get_max_threads();
            else
                threads = std::min(2, omp_get_max_threads());
#endif

#if HAVE_MPI
            MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
#endif

            std::cout << "Using "<< mpiSize << " MPI processes with "<< threads <<" OMP threads on each \n\n";
        }

        /// This is the main function of Flow.  It runs a complete simulation with the
        /// given grid and simulator classes, based on the user-specified command-line
        /// input.
        int execute(int argc, char** argv, bool output_cout, bool output_to_files)
        {
            try {
                // deal with some administrative boilerplate

                int status = setupParameters_(argc, argv);
                if (status)
                    return status;

                setupParallelism();
                {
                    std::cout << "Running simulator setup code" << std::endl;
                    auto start_time = std::chrono::system_clock::now();
                    setupEbosSimulator(output_cout);
                    auto setup_time = std::chrono::system_clock::now() - start_time;
                    std::cout << "Simulator setup time: " << std::chrono::duration<double>(setup_time).count() << " seconds" << std::endl;
                    std::exit(0);
                }
                runDiagnostics(output_cout);
                createSimulator();

                // do the actual work
                runSimulator(output_cout);

                // clean up
                mergeParallelLogFiles(output_to_files);

                return EXIT_SUCCESS;
            }
            catch (const std::exception& e) {
                std::ostringstream message;
                message  << "Program threw an exception: " << e.what();

                if (output_cout) {
                    // in some cases exceptions are thrown before the logging system is set
                    // up.
                    if (OpmLog::hasBackend("STREAMLOG")) {
                        OpmLog::error(message.str());
                    }
                    else {
                        std::cout << message.str() << "\n";
                    }
                }

                return EXIT_FAILURE;
            }
        }

        // Print an ASCII-art header to the PRT and DEBUG files.
        // \return Whether unkown keywords were seen during parsing.
        static void printPRTHeader(bool output_cout)
        {
          if (output_cout) {
              const std::string version = moduleVersion();
              const double megabyte = 1024 * 1024;
              unsigned num_cpu = std::thread::hardware_concurrency();
              struct utsname arch;
              const char* user = getlogin();
              time_t now = std::time(0);
              struct tm  tstruct;
              char      tmstr[80];
              tstruct = *localtime(&now);
              strftime(tmstr, sizeof(tmstr), "%d-%m-%Y at %X", &tstruct);
              const double mem_size = getTotalSystemMemory() / megabyte;
              std::ostringstream ss;
              ss << "\n\n\n";
              ss << " ########  #          ######   #           #\n";
              ss << " #         #         #      #   #         # \n";
              ss << " #####     #         #      #    #   #   #  \n";
              ss << " #         #         #      #     # # # #   \n";
              ss << " #         #######    ######       #   #    \n\n";
              ss << "Flow is a simulator for fully implicit three-phase black-oil flow,";
              ss << " and is part of OPM.\nFor more information visit: https://opm-project.org \n\n";
              ss << "Flow Version     =  " + version + "\n";
              if (uname(&arch) == 0) {
                 ss << "Machine name     =  " << arch.nodename << " (Number of logical cores: " << num_cpu;
                 ss << ", Memory size: " << std::fixed << std::setprecision (2) << mem_size << " MB) \n";
                 ss << "Operating system =  " << arch.sysname << " " << arch.machine << " (Kernel: " << arch.release;
                 ss << ", " << arch.version << " )\n";
                 }
              if (user) {
                 ss << "User             =  " << user << std::endl;
                 }
              ss << "Simulation started on " << tmstr << " hrs\n";

              ss << "Parameters used by Flow:\n";
              Opm::Parameters::printValues<TypeTag>(ss);

              OpmLog::note(ss.str());
          }
        }

    protected:
        void setupParallelism()
        {
            // determine the rank of the current process and the number of processes
            // involved in the simulation. MPI must have already been initialized
            // here. (yes, the name of this method is misleading.)
#if HAVE_MPI
            MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank_);
            MPI_Comm_size(MPI_COMM_WORLD, &mpi_size_);
#else
            mpi_rank_ = 0;
            mpi_size_ = 1;
#endif

#if _OPENMP
            // if openMP is available, default to 2 threads per process.
            if (!getenv("OMP_NUM_THREADS"))
                omp_set_num_threads(std::min(2, omp_get_num_procs()));
#endif

            typedef typename GET_PROP_TYPE(TypeTag, ThreadManager) ThreadManager;
            ThreadManager::init();
        }



        void mergeParallelLogFiles(bool output_to_files)
        {
            // force closing of all log files.
            OpmLog::removeAllBackends();

            if (mpi_rank_ != 0 || mpi_size_ < 2 || !output_to_files) {
                return;
            }

            namespace fs = boost::filesystem;
            const std::string& output_dir = eclState().getIOConfig().getOutputDir();
            fs::path output_path(output_dir);
            fs::path deck_filename(EWOMS_GET_PARAM(TypeTag, std::string, EclDeckFileName));
            std::string basename;
            // Strip extension "." and ".DATA"
            std::string extension = boost::to_upper_copy(deck_filename.extension().string());
            if ( extension == ".DATA" || extension == "." )
            {
                basename = boost::to_upper_copy(deck_filename.stem().string());
            }
            else
            {
                basename = boost::to_upper_copy(deck_filename.filename().string());
            }
            std::for_each(fs::directory_iterator(output_path),
                          fs::directory_iterator(),
                          detail::ParallelFileMerger(output_path, basename,
                                                     EWOMS_GET_PARAM(TypeTag, bool, EnableLoggingFalloutWarning)));
        }

        void setupEbosSimulator(bool output_cout)
        {
            {
                auto start_time = std::chrono::system_clock::now();
                ebosSimulator_.reset(new EbosSimulator(/*verbose=*/false));
                auto new_time = std::chrono::system_clock::now() - start_time;
                std::cout << "Simulator creation time: " << std::chrono::duration<double>(new_time).count() << " seconds" << std::endl;
            }


            ebosSimulator_->executionTimer().start();
            {
                auto start_time = std::chrono::system_clock::now();
                ebosSimulator_->model().applyInitialSolution();
                auto init_time = std::chrono::system_clock::now() - start_time;
                std::cout << "Applyinitial Solution time: " << std::chrono::duration<double>(init_time).count() << " seconds" << std::endl;
            }
            try {
                if (output_cout) {
                    MissingFeatures::checkKeywords(deck());
                }

                // Possible to force initialization only behavior (NOSIM).
                const std::string& dryRunString = EWOMS_GET_PARAM(TypeTag, std::string, EnableDryRun);
                if (dryRunString != "" && dryRunString != "auto") {
                    bool yesno;
                    if (dryRunString == "true"
                        || dryRunString == "t"
                        || dryRunString == "1")
                        yesno = true;
                    else if (dryRunString == "false"
                             || dryRunString == "f"
                             || dryRunString == "0")
                        yesno = false;
                    else
                        throw std::invalid_argument("Invalid value for parameter EnableDryRun: '"
                                                    +dryRunString+"'");
                    auto& ioConfig = eclState().getIOConfig();
                    ioConfig.overrideNOSIM(yesno);
                }
            }
            catch (const std::invalid_argument& e) {
                std::cerr << "Failed to create valid EclipseState object" << std::endl;
                std::cerr << "Exception caught: " << e.what() << std::endl;
                throw;
            }
        }

        const Deck& deck() const
        { return ebosSimulator_->vanguard().deck(); }

        Deck& deck()
        { return ebosSimulator_->vanguard().deck(); }

        const EclipseState& eclState() const
        { return ebosSimulator_->vanguard().eclState(); }

        EclipseState& eclState()
        { return ebosSimulator_->vanguard().eclState(); }

        const Schedule& schedule() const
        { return ebosSimulator_->vanguard().schedule(); }


        // Run diagnostics.
        // Writes to:
        //   OpmLog singleton.
        void runDiagnostics(bool output_cout)
        {
            if (!output_cout) {
                return;
            }

            // Run relperm diagnostics if we have more than one phase.
            if (FluidSystem::numActivePhases() > 1) {
                RelpermDiagnostics diagnostic;
                diagnostic.diagnosis(eclState(), deck(), this->grid());
            }
        }

        // Run the simulator.
        void runSimulator(bool output_cout)
        {
            const auto& schedule = this->schedule();
            const auto& timeMap = schedule.getTimeMap();
            auto& ioConfig = eclState().getIOConfig();
            SimulatorTimer simtimer;

            // initialize variables
            const auto& initConfig = eclState().getInitConfig();
            simtimer.init(timeMap, (size_t)initConfig.getRestartStep());

            if (output_cout) {
                std::ostringstream oss;

                // This allows a user to catch typos and misunderstandings in the
                // use of simulator parameters.
                if (Opm::Parameters::printUnused<TypeTag>(oss)) {
                    std::cout << "-----------------   Unrecognized parameters:   -----------------\n";
                    std::cout << oss.str();
                    std::cout << "----------------------------------------------------------------" << std::endl;
                }
            }

            if (!ioConfig.initOnly()) {
                if (output_cout) {
                    std::string msg;
                    msg = "\n\n================ Starting main simulation loop ===============\n";
                    OpmLog::info(msg);
                }

                SimulatorReport successReport = simulator_->run(simtimer);
                SimulatorReport failureReport = simulator_->failureReport();
                if (output_cout) {
                    std::ostringstream ss;
                    ss << "\n\n================    End of simulation     ===============\n\n";
                    ss << "Number of MPI processes: " << std::setw(6) << mpi_size_ << "\n";
#if _OPENMP
                    int threads = omp_get_max_threads();
#else
                    int threads = 1;
#endif
                    ss << "Threads per MPI process:  " << std::setw(5) << threads << "\n";
                    successReport.reportFullyImplicit(ss, &failureReport);
                    OpmLog::info(ss.str());
                }

            } else {
                if (output_cout) {
                    std::cout << "\n\n================ Simulation turned off ===============\n" << std::flush;
                }

            }
        }

        /// This is the main function of Flow.
        // Create simulator instance.
        // Writes to:
        //   simulator_
        void createSimulator()
        {
            // Create the simulator instance.
            simulator_.reset(new Simulator(*ebosSimulator_));
        }

        static unsigned long long getTotalSystemMemory()
        {
            long pages = sysconf(_SC_PHYS_PAGES);
            long page_size = sysconf(_SC_PAGE_SIZE);
            return pages * page_size;
        }


        Grid& grid()
        { return ebosSimulator_->vanguard().grid(); }

    private:
        std::unique_ptr<EbosSimulator> ebosSimulator_;
        int  mpi_rank_ = 0;
        int  mpi_size_ = 1;
        boost::any parallel_information_;
        std::unique_ptr<Simulator> simulator_;
    };
} // namespace Opm

#endif // OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED
