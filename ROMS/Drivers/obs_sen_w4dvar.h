      MODULE ocean_control_mod
!
!svn $Id: obs_sen_w4dvar.h 975 2009-05-05 22:51:13Z kate $
!=================================================== Andrew M. Moore ===
!  Copyright (c) 2002-2009 The ROMS/TOMS Group      Hernan G. Arango   !
!    Licensed under a MIT/X style license                              !
!    See License_ROMS.txt                                              !
!=======================================================================
!                                                                      !
!  ROMS/TOMS Weak Constraint 4-Dimensional Variational (4DVar) Data    !
!         Assimilation and Observation Sensitivity Driver: Indirect    !
!         Representer Approach                                         !
!                                                                      !
!  This driver is used for  weak constraint 4DVar where errors are     !
!  considered in both model and observations. It also computes the     !
!  the sensitivity of the assimilation system to each observation.     !
!  It measures the degree to which each observation contributes to     !
!  the uncertainty in the estimate.  This analysis  can be used to     !
!  determine the type of measurements that need to be made,  where     !
!  to observe, and when.                                               !
!                                                                      !
!  The routines in this driver control the initialization,  time-      !
!  stepping, and finalization of  ROMS/TOMS  model following ESMF      !
!  conventions:                                                        !
!                                                                      !
!     ROMS_initialize                                                  !
!     ROMS_run                                                         !
!     ROMS_finalize                                                    !
!                                                                      !
!=======================================================================
!
      implicit none

      PRIVATE
      PUBLIC  :: ROMS_initialize
      PUBLIC  :: ROMS_run
      PUBLIC  :: ROMS_finalize

      CONTAINS

      SUBROUTINE ROMS_initialize (first, MyCOMM)
!
!=======================================================================
!                                                                      !
!  This routine allocates and initializes ROMS/TOMS state variables    !
!  and internal and external parameters.                               !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
      USE mod_fourdvar
      USE mod_iounits
      USE mod_scalars
!
#ifdef AIR_OCEAN 
      USE ocean_coupler_mod, ONLY : initialize_atmos_coupling
#endif
#ifdef WAVES_OCEAN
      USE ocean_coupler_mod, ONLY : initialize_waves_coupling
#endif
!
!  Imported variable declarations.
!
      logical, intent(inout) :: first

      integer, intent(in), optional :: MyCOMM
!
!  Local variable declarations.
!
      logical :: allocate_vars = .TRUE.

      integer :: STDrec, Tindex, ng, thread

#ifdef DISTRIBUTE
!
!-----------------------------------------------------------------------
!  Set distribute-memory (MPI) world communictor.
!-----------------------------------------------------------------------
!
      IF (PRESENT(MyCOMM)) THEN
        OCN_COMM_WORLD=MyCOMM
      ELSE
        OCN_COMM_WORLD=MPI_COMM_WORLD
      END IF
#endif
!
!-----------------------------------------------------------------------
!  On first pass, initialize model parameters a variables for all
!  nested/composed grids.  Notice that the logical switch "first"
!  is used to allow multiple calls to this routine during ensemble
!  configurations.
!-----------------------------------------------------------------------
!
      IF (first) THEN
        first=.FALSE.
!
!  Initialize parallel parameters.
!
        CALL initialize_parallel
!
!  Initialize wall clocks.
!
        IF (Master) THEN
          WRITE (stdout,10)
 10       FORMAT (' Process Information:',/)
        END IF
        DO ng=1,Ngrids
!$OMP PARALLEL DO PRIVATE(thread) SHARED(ng,numthreads)
          DO thread=0,numthreads-1
            CALL wclock_on (ng, iNLM, 0)
          END DO
!$OMP END PARALLEL DO
        END DO

#if defined AIR_OCEAN || defined WAVES_OCEAN
!
!  Initialize coupling streams between model(s).
!
        DO ng=1,Ngrids
# ifdef AIR_OCEAN
          CALL initialize_atmos_coupling (ng, MyRank)
# endif
# ifdef WAVES_OCEAN
          CALL initialize_waves_coupling (ng, MyRank)
# endif
        END DO
#endif
!
!  Read in model tunable parameters from standard input. Initialize
!  "mod_param", "mod_ncparam" and "mod_scalar" modules.
!
        CALL inp_par (iNLM)
        IF (exit_flag.ne.NoError) RETURN
!
!  Allocate and initialize modules variables.
!
        CALL mod_arrays (allocate_vars)
!
!  Allocate and initialize observation arrays.
!
        CALL initialize_fourdvar
!
!  Read in standard deviation factors for initial conditions
!  error covariance.  They are loaded in Tindex=1 of the
!  e_var(...,Tindex) state variables.
!
        STDrec=1
        Tindex=1
        DO ng=1,Ngrids
          CALL get_state (ng, 6, 6, STDname(1,ng), STDrec, Tindex)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Read in standard deviation factors for model error covariance.
!  They are loaded in Tindex=2 of the e_var(...,Tindex) state
!  variables.
!
        STDrec=1
        Tindex=2
        DO ng=1,Ngrids
          IF (NSA.eq.2) THEN
            CALL get_state (ng, 6, 6, STDname(2,ng), STDrec, Tindex)
            IF (exit_flag.ne.NoError) RETURN
          END IF
        END DO

#ifdef ADJUST_BOUNDARY
!
!  Read in standard deviation factors for boundary conditions
!  error covariance.
!
        STDrec=1
        Tindex=1
        DO ng=1,Ngrids
          CALL get_state (ng, 8, 8, STDname(3,ng), STDrec, Tindex)
          IF (exit_flag.ne.NoError) RETURN
        END DO
#endif
#if defined ADJUST_WSTRESS || defined ADJUST_STFLUX
!
!  Read in standard deviation factors for surface forcing
!  error covariance.
!
        STDrec=1
        Tindex=1
        DO ng=1,Ngrids
          CALL get_state (ng, 9, 9, STDname(4,ng), STDrec, 1)
          IF (exit_flag.ne.NoError) RETURN
        END DO
#endif
      END IF

      RETURN
      END SUBROUTINE ROMS_initialize

      SUBROUTINE ROMS_run (Tstr, Tend)
!
!=======================================================================
!                                                                      !
!  This routine time-steps ROMS/TOMS nonlinear, tangent linear and     !
!  adjoint models.                                                     !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
      USE mod_fourdvar
      USE mod_iounits
      USE mod_ncparam
      USE mod_netcdf
      USE mod_scalars
      USE mod_stepping
!
#ifdef BALANCE_OPERATOR
      USE ad_balance_mod, ONLY: ad_balance
#endif
      USE ad_convolution_mod, ONLY : ad_convolution
      USE ad_variability_mod, ONLY : ad_variability
      USE ini_adjust_mod, ONLY : rp_ini_adjust
      USE ini_adjust_mod, ONLY : load_ADtoTL
      USE ini_adjust_mod, ONLY : load_TLtoAD
#if defined ADJUST_STFLUX || defined ADJUST_WSTRESS
      USE mod_forces, ONLY : initialize_forces
#endif
#ifdef ADJUST_BOUNDARY
      USE mod_boundary, ONLY : initialize_boundary
#endif
      USE mod_ocean, ONLY : initialize_ocean
      USE normalization_mod, ONLY : normalization
      USE mod_forces, ONLY : initialize_forces
      USE strings_mod, ONLY : uppercase
#ifdef BALANCE_OPERATOR
      USE tl_balance_mod, ONLY: tl_balance
#endif
      USE tl_convolution_mod, ONLY : tl_convolution
      USE tl_variability_mod, ONLY : tl_variability
!
!  Imported variable declarations
!
      integer, dimension(Ngrids) :: Tstr
      integer, dimension(Ngrids) :: Tend
!
!  Local variable declarations.
!
      logical :: Lcgini, Linner, Lweak, add

      integer :: my_inner, my_outer
      integer :: ADrec, Lbck, Lini, Nrec, Rec1, Rec2
      integer :: i, lstr, my_iic, ng, rec, status, subs, tile, thread

      real(r8) :: MyTime, LB_time, UB_time

      character (len=20) :: string
!
!=======================================================================
!  Run model for all nested grids, if any.
!=======================================================================
!
      NEST_LOOP : DO ng=1,Ngrids
!
!  Initialize relevant parameters.
!
#if defined ADJUST_STFLUX || defined ADJUST_WSTRESS
        Lfinp(ng)=1         ! forcing index for input
        Lfout(ng)=1         ! forcing index for output history files
#endif
        Lold(ng)=1          ! old minimization time index
        Lnew(ng)=2          ! new minimization time index
        Lini=1              ! NLM initial conditions record in INIname
        Lbck=2              ! background record in INIname
        Rec1=1
        Rec2=2 
        Nrun=1
        outer=0
        inner=0
        ERstr=1
        ERend=Nouter
!
!-----------------------------------------------------------------------
!  Configure weak constraint 4DVAR algorithm: Indirect Representer
!  Approach.
!-----------------------------------------------------------------------
!
!  Initialize the switch to gather weak constraint forcing.
!
        WRTforce(ng)=.FALSE.
!
!  Initialize and set nonlinear model initial conditions.
!
        wrtNLmod(ng)=.TRUE.
        wrtRPmod(ng)=.FALSE.
        wrtTLmod(ng)=.FALSE.
        CALL initial (ng)
        IF (exit_flag.ne.NoError) RETURN
!
!  Save nonlinear initial conditions (currently in time index 1,
!  background) into record "Lini" of INIname NetCDF file. The record
!  "Lbck" becomes the background state record and the record "Lini"
!  becomes current nonlinear initial conditions.
!
        tINIindx(ng)=1
        NrecINI(ng)=1
        CALL wrt_ini (ng, 1)
        IF (exit_flag.ne.NoError) RETURN
!
!  Set nonlinear output history file as the initial basic state
!  trajectory.
!
        LdefHIS(ng)=.TRUE.
        LwrtHIS(ng)=.TRUE.
        lstr=LEN_TRIM(FWDbase(ng))
        WRITE (HISname(ng),10) FWDbase(ng)(1:lstr-3), outer
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Model-error covariance normalization and stardard deviation factors.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Compute or read in the error correlation normalization factors.
!  If computing, write out factors to NetCDF. This is an expensive
!  computation that needs to be computed only once for a particular
!  application grid and decorrelation scales.
!  
        IF (ANY(LwrtNRM(:,ng))) THEN
          CALL def_norm (ng, iNLM, 1)
          IF (exit_flag.ne.NoError) RETURN

          IF (NSA.eq.2) THEN
            CALL def_norm (ng, iNLM, 2)
          IF (exit_flag.ne.NoError) RETURN
          END IF
#ifdef ADJUST_BOUNDARY
          CALL def_norm (ng, iNLM, 3)
          IF (exit_flag.ne.NoError) RETURN
#endif
#if defined ADJUST_WSTRESS || defined ADJUST_STFLUX
          CALL def_norm (ng, iNLM, 4)
          IF (exit_flag.ne.NoError) RETURN
#endif
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile) SHARED(numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1
              CALL normalization (ng, TILE, 2)
            END DO
          END DO
!$OMP END PARALLEL DO
          LdefNRM(1:4,ng)=.FALSE.
          LwrtNRM(1:4,ng)=.FALSE.
        ELSE
          CALL get_state (ng, 5, 5, NRMname(1,ng), 1, 1)
          IF (exit_flag.ne.NoError) RETURN

          IF (NSA.eq.2) THEN
            CALL get_state (ng, 5, 5, NRMname(2,ng), 1, 2)
            IF (exit_flag.ne.NoError) RETURN
          END IF
#ifdef ADJUST_BOUNDARY
          CALL get_state (ng, 10, 10, NRMname(3,ng), 1, 1)
          IF (exit_flag.ne.NoError) RETURN
#endif
#if defined ADJUST_WSTRESS || defined ADJUST_STFLUX
          CALL get_state (ng, 11, 11, NRMname(4,ng), 1, 1)
          IF (exit_flag.ne.NoError) RETURN
#endif
        END IF
!
!  Define tangent linear initial conditions file.
!
        LdefITL(ng)=.TRUE.
        CALL tl_def_ini (ng)
        IF (exit_flag.ne.NoError) RETURN
!
!  Define TLM/RPM impulse forcing NetCDF file.
!
        LdefTLF(ng)=.TRUE.
        CALL def_impulse (ng)
        IF (exit_flag.ne.NoError) RETURN
!
!  Define output 4DVAR NetCDF file containing all processed data
!  at observation locations.
!
        LdefMOD(ng)=.TRUE.
        CALL def_mod (ng)
        IF (exit_flag.ne.NoError) RETURN
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Run nonlinear model and compute basic state trajectory.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
        IF (Master) THEN
          WRITE (stdout,20) 'NL', ntstart(ng), ntend(ng)
        END IF

        time(ng)=time(ng)-dt(ng)

        NL_LOOP : DO my_iic=ntstart(ng),ntend(ng)+1

          iic(ng)=my_iic
#ifdef SOLVE3D
          CALL main3d (ng)
#else
          CALL main2d (ng)
#endif
          IF (exit_flag.ne.NoError) RETURN

        END DO NL_LOOP
        wrtNLmod(ng)=.FALSE.
!
!  Set forward basic state NetCDF ID to nonlinear model trajectory to
!  avoid the inquiring stage. 
!
        ncFWDid(ng)=ncHISid(ng)
!
!-----------------------------------------------------------------------
!  Solve the system:
!
!              (R_n + Cobs) * Beta_n = h_n
!
!              h_n = Xo - H * X_n
!
!  where R_n is the representer matrix, Cobs is the observation-error
!  covariance, Beta_n are the representer coefficients, h_n is the
!  misfit between observations (Xo) and model (H * X_n), and H is
!  the linearized observation operator. Here, _n denotes a sequence
!  of estimates.
!
!  The system does not need to be solved explicitly by inverting the
!  symmetric stabilized representer matrix, P_n:
!
!              P_n = R_n + Cobs
!
!  but by computing the action of P_n on any vector PSI, such that
!
!              P_n * PSI = R_n * PSI + Cobs * PSI
!
!  The representer matrix is not explicitly computed but evaluated by
!  one integration backward of the adjoint model and one integration
!  forward of the tangent linear model for any forcing vector PSI.
!
!  A preconditioned conjugate gradient algorithm is used to compute
!  an approximation PSI for Beta_n.
!
!-----------------------------------------------------------------------
!
        OUTER_LOOP : DO my_outer=1,1
          outer=my_outer
          inner=0
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Run representer model and compute a "prior estimate" state
!  trajectory, X_n(t). Use linearized state trajectory (X_n-1) as
!  basic state.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Set representer model basic state trajectory file to previous outer
!  loop file (outer-1). If outer=1, the basic state trajectory is the
!  nonlinear model. 
!
          lstr=LEN_TRIM(FWDbase(ng))
          WRITE (FWDname(ng),10) FWDbase(ng)(1:lstr-3), outer-1
!
!  Set representer model output file name.  The strategy is to write
!  the representer solution at the beginning of each outer loop.
!
          LdefTLM(ng)=.TRUE.
          LwrtTLM(ng)=.TRUE.
          lstr=LEN_TRIM(TLMbase(ng))
          WRITE (TLMname(ng),10) TLMbase(ng)(1:lstr-3), outer
!
!  Activate switch to write the representer model at observation points.
!  Turn off writing into history file and turn off impulse forcing.
!
          wrtRPmod(ng)=.TRUE.
          SporadicImpulse=.FALSE.
          FrequentImpulse=.FALSE.

#ifndef DATALESS_LOOPS
!
!  As in the nonlinear model, initialize always the representer model
!  here with the background or reference state (IRPname, record Rec1).
!
          tIRPindx(ng)=Rec1
          CALL rp_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
!
!  Run representer model using the nonlinear trajectory as a basic
!  state.  Compute model solution at observation points, H * X_n.
!
          IF (Master) THEN
            WRITE (stdout,20) 'RP', ntstart(ng), ntend(ng)
          END IF

          time(ng)=time(ng)-dt(ng)

          RP_LOOP1 : DO my_iic=ntstart(ng),ntend(ng)+1

            iic(ng)=my_iic
# ifdef SOLVE3D
            CALL rp_main3d (ng)
# else
            CALL rp_main2d (ng)
# endif
            IF (exit_flag.ne.NoError) RETURN

          END DO RP_LOOP1
!
!  Report data penalty function. Then, clean array before next run of
!  RP model.
!
          IF (Master) THEN
            DO i=0,NstateVar(ng)
              IF (i.eq.0) THEN
                string='Total'
              ELSE
                string=Vname(1,idSvar(i))
              END IF
              IF (FOURDVAR(ng)%DataPenalty(i).ne.0.0_r8) THEN
                WRITE (stdout,30) outer, inner, 'RPM',                  &
     &                            FOURDVAR(ng)%DataPenalty(i),          &
     &                            TRIM(string)
              END IF
            END DO
          END IF
          FOURDVAR(ng)%DataPenalty=0.0_r8
!
!  Turn off IO switches.
!
          LdefTLM(ng)=.FALSE.
          LwrtTLM(ng)=.FALSE.
          wrtRPmod(ng)=.FALSE.
!
!  Clear tangent linear forcing arrays before entering inner-loop.
!  This is very important since these arrays are non-zero after
!  running the representer model and must be zero when running the
!  tangent linear model.
!
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile) SHARED(numthreads)
          DO thread=0,numthreads-1
# if defined _OPENMP || defined DISTRIBUTE
            subs=NtileX(ng)*NtileE(ng)/numthreads
# else
            subs=1
# endif
            DO tile=subs*thread,subs*(thread+1)-1
              CALL initialize_forces (ng, TILE, iTLM)
# ifdef ADJUST_BOUNDARY
              CALL initialize_boundary (ng, TILE, iTLM)
# endif
            END DO
          END DO
!$OMP END PARALLEL DO
!
          INNER_LOOP : DO my_inner=0,Ninner
            inner=my_inner
!
! Initialize conjugate gradient algorithm depending on hot start or
! outer loop index.
!
            IF (inner.eq.0) THEN
              Lcgini=.TRUE.
              CALL congrad (ng, iRPM, outer, inner, Ninner, Lcgini)
            END IF
!
!  If initialization step, skip the inner-loop computations.
!
            Linner=.FALSE.
            IF ((inner.ne.0).or.(Nrun.ne.1)) THEN
              IF (((inner.eq.0).and.LhotStart).or.(inner.ne.0)) THEN
                Linner=.TRUE.
              END IF
            END IF
!
!  Start inner loop computations.
!
            INNER_COMPUTE : IF (Linner) THEN
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate adjoint model forced with any vector PSI at the observation
!  locations and generate adjoint trajectory, Lambda_n(t).
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Initialize the adjoint model from rest.
!
              LsenPSAS(ng)=.FALSE.
              CALL ad_initial (ng)
              IF (exit_flag.ne.NoError) RETURN
              wrtMisfit(ng)=.FALSE.

# ifdef RPM_RELAXATION
!
!  Adjoint of representer relaxation is not applied during the 
!  inner-loops.
!
              LweakRelax(ng)=.FALSE.
# endif
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file only once to avoid opening too many files.
!
              WRTforce(ng)=.TRUE.
              IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
              NrecADJ(ng)=0
              tADJindx(ng)=0
!
!  Time-step adjoint model backwards forced with current PSI vector.
!
              IF (Master) THEN
                WRITE (stdout,20) 'AD', ntstart(ng), ntend(ng)
              END IF

              time(ng)=time(ng)+dt(ng)

              AD_LOOP1 : DO my_iic=ntstart(ng),ntend(ng),-1

                iic(ng)=my_iic
# ifdef SOLVE3D
                CALL ad_main3d (ng)
# else
                CALL ad_main2d (ng)
# endif
                IF (exit_flag.ne.NoError) RETURN

              END DO AD_LOOP1
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps.
!
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
              WRTforce(ng)=.FALSE.
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN

# ifdef CONVOLVE
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Convolve adjoint trajectory with error covariances and convert
!  to impulse forcing.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
              Nrec=NrecADJ(ng)
              NrecADJ(ng)=0
              tADJindx(ng)=0
              LwrtState2d(ng)=.TRUE.
              LwrtTime(ng)=.FALSE.
              IF (Master) THEN
                WRITE (stdout,40) outer, inner
              END IF
!
!  Clear adjoint state arrays.
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile), SHARED(numthreads)
              DO thread=0,numthreads-1
                subs=NtileX(ng)*NtileE(ng)/numthreads
                DO tile=subs*thread,subs*(thread+1)-1
                  CALL initialize_ocean (ng, TILE, iADM)
                END DO
              END DO
!$OMF END PARALLEL DO
!
!  Convolve initial conditions record and adjoint forcing
!  (ADJname, record Nrec) with  initial conditions background error 
!  covariance. Note that we only do this for the forcing in 
!  record Nrec since this is the only record for which
!  the adjoing forcing arrays are complete. Since routine
!  "get_state" loads data into the ghost points, the adjoint
!  solution is read into the tangent linear state arrays by using
!  iTLM instead of iADM in the calling arguments.
!
              ADrec=Nrec
              FrcRec(ng)=Nrec
              Lweak=.FALSE.
              CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec, Lold(ng))
              IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays. 
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the 
!  squared-root adjoint diffusion operator which impose initial
!  conditions background error covaraince. Notice that the spatial
!  convolution is only done for half of the diffusion steps
!  (squared-root filter). Clear tangent linear state arrays when
!  done.
!
              add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
              DO thread=0,numthreads-1
                subs=NtileX(ng)*NtileE(ng)/numthreads
                DO tile=subs*thread,subs*(thread+1)-1
                  CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
#  ifdef BALANCE_OPERATOR_NOT_YET
                  CALL ad_balance (ng, TILE, Lini, Lold(ng))
#  endif
                  CALL ad_variability (ng, TILE, Lold(ng), Lweak)
                  CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
                  CALL initialize_ocean (ng, TILE, iTLM)
                  CALL initialize_forces (ng, TILE, iTLM)
                END DO
              END DO
!$OMP END PARALLEL DO
!
!  To insure symmetry, convolve resulting filtered adjoint solution
!  from above with the squared-root (half of steps) tangent linear
!  diffusion operator. Then, multiply result with its corresponding
!  background-error standard deviations. Since the convolved solution
!  is in the adjoint state arrays, first copy to tangent linear state
!  arrays including the ghosts points.
!
              add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
              DO thread=0,numthreads-1
                subs=NtileX(ng)*NtileE(ng)/numthreads
                DO tile=subs*thread,subs*(thread+1)-1,+1
                  CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng), add)
                  CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
                  CALL tl_variability (ng, TILE, Lold(ng), Lweak)
#  ifdef BALANCE_OPERATOR_NOT_YET
                  CALL tl_balance (ng, TILE, Lini, Lold(ng))
#  endif
                END DO
              END DO
!$OMP END PARALLEL DO
!
!  Write out tangent linear model initial conditions and tangent
!  linear surface forcing adjustments for next inner
!  loop into ITLname (record Rec1). The tangent model initial
!  conditions are set to the convolved adjoint solution.
!
              CALL tl_wrt_ini (ng, Lold(ng), Rec1) 
              IF (exit_flag.ne.NoError) RETURN
!
!  If weak constraint, convolve records 2-Nrec in ADJname and
!  impose model error covariance. NOTE: We will not use the
!  convolved forcing increments generated here since these arrays
!  do not contain the complete solution and are redundant.
!  AMM: We might want to get rid of these unwanted records to
!  avoid any confusion in the future.
!
              IF (Nrec.gt.3) THEN
                LwrtTime(ng)=.TRUE.
                DO rec=1,Nrec-1
                  Lweak=.TRUE.
!
!  Read adjoint solution. Since routine "get_state" loads data into the
!  ghost points, the adjoint solution is read in the tangent linear
!  state arrays by using iTLM instead of iADM in the calling arguments.
!
                  ADrec=rec
                  CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec,      &
     &                            Lold(ng))
                  IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays. 
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the 
!  squared-root adjoint diffusion operator which impose the model-error
!  spatial correlations. Notice that the spatial convolution is only
!  done for half of the diffusion steps (squared-root filter). Clear
!  tangent linear state arrays when done.
!
                  add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
                  DO thread=0,numthreads-1
                    subs=NtileX(ng)*NtileE(ng)/numthreads
                    DO tile=subs*thread,subs*(thread+1)-1
                      CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng),   &
     &                                  add)
#  ifdef BALANCE_OPERATOR_NOT_YET
                      CALL ad_balance (ng, TILE, Lini, Lold(ng))
#  endif
                      CALL ad_variability (ng, TILE, Lold(ng), Lweak)
                      CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
                      CALL initialize_ocean (ng, TILE, iTLM)
                      CALL initialize_forces (ng, TILE, iTLM)
                    END DO
                  END DO
!$OMP END PARALLEL DO
!
!  To insure symmetry, convolve resulting filtered adjoint solution
!  from above with the squared-root (half of steps) tangent linear
!  diffusion operator. Then, multiply result with its corresponding
!  background-error standard deviations.  Since the convolved solution
!  is in the adjoint state arrays, first copy to tangent linear state
!  arrays including the ghosts points. Copy back to adjoint state
!  arrays when done with the convolution for output purposes.
!
                  add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
                  DO thread=0,numthreads-1
                    subs=NtileX(ng)*NtileE(ng)/numthreads
                    DO tile=subs*thread,subs*(thread+1)-1,+1
                      CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng),   &
     &                                  add)
                      CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
                      CALL tl_variability (ng, TILE, Lold(ng), Lweak)
#  ifdef BALANCE_OPERATOR_NOT_YET
                      CALL tl_balance (ng, TILE, Lini, Lold(ng))
#  endif
                      CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng),   &
     &                                  add)
                    END DO
                  END DO
!$OMP END PARALLEL DO
!
!  Overwrite ADJname history NetCDF file with convolved adjoint
!  solution.
!
                  kstp(ng)=Lold(ng)
#  ifdef SOLVE3D
                  nstp(ng)=Lold(ng)
#  endif
                  CALL ad_wrt_his (ng)
                  IF (exit_flag.ne.NoError) RETURN
                END DO
                LwrtState2d(ng)=.FALSE.
                LwrtTime(ng)=.TRUE.
              END IF
# endif
!
!  Convert the current adjoint solution in ADJname to impulse forcing.
!  Write out impulse forcing into TLFname NetCDF file. To facilitate
!  the forcing to the TLM and RPM, the forcing is processed and written
!  in increasing time coordinates (recall that the adjoint solution
!  in ADJname is backwards in time).
!
              IF (Master) THEN
                WRITE (stdout,50) outer, inner
              END IF
              tTLFindx(ng)=0
# ifdef DISTRIBUTE
              tile=MyRank
# else
              tile=-1
# endif
              CALL wrt_impulse (ng, tile, iADM, ADJname(ng))
              IF (exit_flag.ne.NoError) RETURN
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate tangent linear model forced by the convolved adjoint
!  trajectory (impulse forcing) to compute R_n * PSI at observation
!  points.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
              TLMname(ng)=TLMbase(ng)
              wrtNLmod(ng)=.FALSE.
              wrtTLmod(ng)=.TRUE.
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
              IF (FrcRec(ng).gt.3) THEN
                FrequentImpulse=.TRUE.
              END IF
!
!  Initialize tangent linear model from ITLname, record Rec1. 
!
              tITLindx(ng)=Rec1
              CALL tl_initial (ng)
              IF (exit_flag.ne.NoError) RETURN
!
!  Activate switch to write out initial misfit between model and
!  observations.
!
              IF ((outer.eq.1).and.(inner.eq.1)) THEN
                wrtMisfit(ng)=.TRUE.
              END IF
!
!  Set tangent linear history NetCDF parameters.  Define tangent linear
!  history file at the beggining of each inner loop  to avoid opening
!  too many NetCDF files.
!
              IF (inner.gt.1) LdefTLM(ng)=.FALSE.
              NrecTLM(ng)=0
              tTLMindx(ng)=0
!
!  Run tangent linear model forward and force with convolved adjoint
!  trajectory impulses. Compute R_n * PSI at observation points which
!  are used in the conjugate gradient algorithm.
!
              IF (Master) THEN
                WRITE (stdout,20) 'TL', ntstart(ng), ntend(ng)
              END IF

              MyTime=time(ng)
              time(ng)=time(ng)-dt(ng)

              TL_LOOP1 : DO my_iic=ntstart(ng),ntend(ng)+1

                iic(ng)=my_iic
# ifdef SOLVE3D
                CALL tl_main3d (ng)
# else
                CALL tl_main2d (ng)
# endif
                MyTime=time(ng)

                IF (exit_flag.ne.NoError) RETURN

              END DO TL_LOOP1
              wrtNLmod(ng)=.FALSE.
              wrtTLmod(ng)=.FALSE.
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Use conjugate gradient algorithm to find a better approximation
!  PSI to representer coefficients Beta_n.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
              Nrun=Nrun+1
              Lcgini=.FALSE.
              CALL congrad (ng, iRPM, outer, inner, Ninner, Lcgini)
              IF (exit_flag.ne.NoError) RETURN

            END IF INNER_COMPUTE

          END DO INNER_LOOP
!
!  Close tangent linear NetCDF file.
!
          SourceFile='w4dvar_ocean.h, ROMS_run'

          CALL netcdf_close (ng, iTLM, ncTLMid(ng))
          IF (exit_flag.ne.NoError) RETURN
!
!-----------------------------------------------------------------------
!  Once that the representer coefficients, Beta_n, have been
!  approximated with sufficient accuracy, compute estimates of
!  Lambda_n and Xhat_n by carrying out one backward intergration
!  of the adjoint model and one forward itegration of the representer
!  model.
!-----------------------------------------------------------------------
!
!  Initialize the adjoint model always from rest.
!
          LsenPSAS(ng)=.FALSE.
          CALL ad_initial (ng)
          IF (exit_flag.ne.NoError) RETURN

# ifdef RPM_RELAXATION
!
!  Adjoint of representer relaxation is applied during the 
!  outer-loops.
!
          LweakRelax(ng)=.TRUE.
# endif
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file one to avoid opening to many files.
!
          WRTforce(ng)=.TRUE.
          IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
          NrecADJ(ng)=0
          tADJindx(ng)=0
!
!  Time-step adjoint model backwards forced with estimated representer
!  coefficients, Beta_n.
!
          IF (Master) THEN
            WRITE (stdout,20) 'AD', ntstart(ng), ntend(ng)
          END IF

          time(ng)=time(ng)+dt(ng)

          AD_LOOP2 : DO my_iic=ntstart(ng),ntend(ng),-1

            iic(ng)=my_iic
# ifdef SOLVE3D
            CALL ad_main3d (ng)
# else
            CALL ad_main2d (ng)
# endif
            IF (exit_flag.ne.NoError) RETURN

          END DO AD_LOOP2
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps.
!
            CALL ad_wrt_his (ng)
            IF (exit_flag.ne.NoError) RETURN
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
            WRTforce(ng)=.FALSE.
            CALL ad_wrt_his (ng)
            IF (exit_flag.ne.NoError) RETURN

# ifdef CONVOLVE
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Convolve adjoint trajectory with model-error covariance and convert
!  to impulse forcing.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
          Nrec=NrecADJ(ng)
          NrecADJ(ng)=0
          tADJindx(ng)=0
          LwrtState2d(ng)=.TRUE.
          LwrtTime(ng)=.FALSE.
          IF (Master) THEN
            WRITE (stdout,40) outer, inner
          END IF
!
!  Clear adjoint state arrays.
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile), SHARED(numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1
              CALL initialize_ocean (ng, TILE, iADM)
            END DO
          END DO
!$OMF END PARALLEL DO
!
!  Convolve initial conditions record (ADJname, record Nrec) with
!  initial conditions background error covariance. Since routine
!  "get_state" loads data into the ghost points, the adjoint
!  solution is read into the tangent linear state arrays by using
!  iTLM instead of iADM in the calling arguments.
!
          ADrec=Nrec
          Lweak=.FALSE.
          CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec, Lold(ng))
          IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays. 
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the 
!  squared-root adjoint diffusion operator which impose initial
!  conditions background error covaraince. Notice that the spatial
!  convolution is only done for half of the diffusion steps
!  (squared-root filter). Clear tangent linear state arrays when
!  done.
!
          add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1
              CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
#  ifdef BALANCE_OPERATOR_NOT_YET
              CALL ad_balance (ng, TILE, Lini, Lold(ng))
#  endif
              CALL ad_variability (ng, TILE, Lold(ng), Lweak)
              CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
              CALL initialize_ocean (ng, TILE, iTLM)
              CALL initialize_forces (ng, TILE, iTLM)
            END DO
          END DO
!$OMP END PARALLEL DO
!
!  To insure symmetry, convolve resulting filtered adjoint solution
!  from above with the squared-root (half of steps) tangent linear
!  diffusion operator. Then, multiply result with its corresponding
!  background-error standard deviations.  Since the convolved solution
!  is in the adjoint state arrays, first copy to tangent linear state
!  arrays including the ghosts points. Copy back to adjoint state
!  arrays when done with the convolution. Compute representer model
!  initial conditions by adding convolved adjoint solution to the
!  reference nonlinear state (INIname, record Lbck).
!
          CALL get_state (ng, iNLM, 9, INIname(ng), Lbck, Lnew(ng))
          IF (exit_flag.ne.NoError) RETURN

          add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1,+1
              CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng), add)
              CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
              CALL tl_variability (ng, TILE, Lold(ng), Lweak)
#  ifdef BALANCE_OPERATOR_NOT_YET
              CALL tl_balance (ng, TILE, Lini, Lold(ng))
#  endif
              CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
              CALL rp_ini_adjust (ng, TILE, Lnew(ng), Lold(ng))
            END DO
          END DO
!$OMP END PARALLEL DO
!
!  Write out representer model initial conditions into IRPname, record
!  Rec2.
!
          CALL rp_wrt_ini (ng, Lold(ng), Rec2) 
          IF (exit_flag.ne.NoError) RETURN
!
!  If weak constraint, convolve adjoint records in ADJname and impose
!  model error covariance.
!
          IF (Nrec.gt.3) THEN
            LwrtTime(ng)=.TRUE.
            DO rec=1,Nrec-1
              Lweak=.TRUE.
!
!  Read adjoint solution. Since routine "get_state" loads data into the
!  ghost points, the adjoint solution is read in the tangent linear
!  state arrays by using iTLM instead of iADM in the calling arguments.
!
              ADrec=rec
              CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec, Lold(ng))
              IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays. 
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the 
!  squared-root adjoint diffusion operator which impose the model-error
!  spatial correlations. Notice that the spatial convolution is only
!  done for half of the diffusion steps (squared-root filter). Clear
!  tangent linear state arrays when done.
!
              add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
              DO thread=0,numthreads-1
                subs=NtileX(ng)*NtileE(ng)/numthreads
                DO tile=subs*thread,subs*(thread+1)-1
                  CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
#  ifdef BALANCE_OPERATOR_NOT_YET
                  CALL ad_balance (ng, TILE, Lini, Lold(ng))
#  endif
                  CALL ad_variability (ng, TILE, Lold(ng), Lweak)
                  CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
                  CALL initialize_ocean (ng, TILE, iTLM)
                  CALL initialize_forces (ng, TILE, iTLM)
                END DO
              END DO
!$OMP END PARALLEL DO
!
!  To insure symmetry, convolve resulting filtered adjoint solution
!  from above with the squared-root (half of steps) tangent linear
!  diffusion operator. Then, multiply result with its corresponding
!  background-error standard deviations.  Since the convolved solution
!  is in the adjoint state arrays, first copy to tangent linear state
!  arrays including the ghosts points. Copy back to adjoint state
!  arrays when done with the convolution for output purposes.
!
              add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
              DO thread=0,numthreads-1
                subs=NtileX(ng)*NtileE(ng)/numthreads
                DO tile=subs*thread,subs*(thread+1)-1,+1
                  CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng), add)
                  CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
                  CALL tl_variability (ng, TILE, Lold(ng), Lweak)
#  ifdef BALANCE_OPERATOR_NOT_YET
                  CALL tl_balance (ng, TILE, Lini, Lold(ng))
#  endif
                  CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
                END DO
              END DO
!$OMP END PARALLEL DO
!
!  Overwrite ADJname history NetCDF file with convolved adjoint
!  solution.
!
              kstp(ng)=Lold(ng)
#  ifdef SOLVE3D
              nstp(ng)=Lold(ng)
#  endif
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
            LwrtState2d(ng)=.FALSE.
            LwrtTime(ng)=.TRUE.
          END IF
# endif
!
!  Convert the current adjoint solution in ADJname to impulse forcing.
!  Write out impulse forcing into TLFname NetCDF file. To facilitate
!  the forcing to the TLM and RPM, the forcing is processed and written
!  in increasing time coordinates (recall that the adjoint solution
!  in ADJname is backwards in time).
!
          IF (Master) THEN
            WRITE (stdout,50) outer, inner
          END IF
          tTLFindx(ng)=0
# ifdef DISTRIBUTE
          tile=MyRank
# else
          tile=-1
# endif
          CALL wrt_impulse (ng, tile, iADM, ADJname(ng))
          IF (exit_flag.ne.NoError) RETURN

#endif /* !DATALESS_LOOPS */
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Run representer model and compute a "new estimate" of the state
!  trajectory, X_n(t).
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Set new basic state trajectory for next outer loop.
!
          LdefTLM(ng)=.TRUE.
          LwrtTLM(ng)=.TRUE.
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.TRUE.
          wrtRPmod(ng)=.TRUE.
          lstr=LEN_TRIM(FWDbase(ng))
          WRITE (TLMname(ng),10) FWDbase(ng)(1:lstr-3), outer
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
          IF (FrcRec(ng).gt.3) THEN
            FrequentImpulse=.TRUE. 
          END IF
!
!  Initialize representer model IRPname file, record Rec2.
!
#ifdef DATALESS_LOOPS
          tIRPindx(ng)=Rec1
#else
          tIRPindx(ng)=Rec2
#endif
          CALL rp_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
!
!  Activate switch to write out final misfit between model and
!  observations.
!
          IF (outer.eq.Nouter) THEN
            wrtMisfit(ng)=.TRUE.
          END IF
!
!  Run representer model using previous linearized trajectory, X_n-1, as
!  basic state and forced with convolved adjoint trajectory impulses.
!
          IF (Master) THEN
            WRITE (stdout,20) 'RP', ntstart(ng), ntend(ng)
          END IF

          time(ng)=time(ng)-dt(ng)

          RP_LOOP2 : DO my_iic=ntstart(ng),ntend(ng)+1

            iic(ng)=my_iic
#ifdef SOLVE3D
            CALL rp_main3d (ng)
#else
            CALL rp_main2d (ng)
#endif
            IF (exit_flag.ne.NoError) RETURN

          END DO RP_LOOP2
!
!  Report data penalty function.
!
          IF (Master) THEN
            DO i=0,NstateVar(ng)
              IF (i.eq.0) THEN
                string='Total'
              ELSE
                string=Vname(1,idSvar(i))
              END IF
              IF (FOURDVAR(ng)%DataPenalty(i).ne.0.0_r8) THEN
                WRITE (stdout,30) outer, inner, 'RPM',                  &
     &                            FOURDVAR(ng)%DataPenalty(i),          &
     &                            TRIM(string)
#ifdef DATALESS_LOOPS
                WRITE (stdout,30) outer, inner, 'NLM',                  &
     &                            FOURDVAR(ng)%NLPenalty(i),            &
     &                            TRIM(string)
#endif
              END IF
            END DO
          END IF
!
!  Write data penalty function to NetCDF file.
!
          SourceFile='w4dvar_ocean.F, ROMS_run'

          CALL netcdf_put_fvar (ng, iRPM, MODname(ng),                  &
     &                          'RPcost_function',                      &
     &                          FOURDVAR(ng)%DataPenalty(0),            &
     &                          (/outer/), (/1/),                       &
     &                          ncid = ncMODid(ng))
          IF (exit_flag.ne.NoError) RETURN
!
!  Clean array before next run of RP model.
!
          FOURDVAR(ng)%DataPenalty=0.0_r8
#ifdef DATALESS_LOOPS
          FOURDVAR(ng)%NLPenalty=0.0_r8
#endif
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.FALSE.
!
!  Close current forward NetCDF file.
!
          CALL netcdf_close (ng, iRPM, ncFWDid(ng))
          IF (exit_flag.ne.NoError) RETURN

        END DO OUTER_LOOP
!
!  Done.  Set history file ID to closed state since we manipulated
!  its indices with the forward file ID which was closed above.
!
        ncHISid(ng)=-1
!!
!! Compute and report model-observation comparison statistics.
!!
!!      CALL stats_modobs (ng)
!!
!
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!  Adjoint of W4DVar to compute the observation sensitivity.
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!
!  WARNING: ONLY 1 outer loop can be used for this application.
!  =======  For more than 1 outer-loop, we require the second
!  derivative of each model operator (i.e. the tangent linear
!  of the tangent linear operator).
!
        AD_OUTER_LOOP : DO my_outer=1,1,-1
          outer=my_outer
          inner=0
!
!-----------------------------------------------------------------------
!  Run the adjoint model initialized and forced by dI/dx where I is the
!  chosen function of the analysis/forecast state x.
!-----------------------------------------------------------------------
!
!  Set basic state trajectory.
!
          lstr=LEN_TRIM(FWDbase(ng))
          WRITE (FWDname(ng),10) FWDbase(ng)(1:lstr-3), outer-1

          IF ((outer.eq.1).and.Master) THEN
            WRITE (stdout,60)
          END IF
!
!  Initialize the adjoint model: initialize using dI/dxf is
!  appropriate.
!
          Lstiffness=.FALSE.
          LsenPSAS(ng)=.TRUE.

          CALL ad_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file one to avoid opening to many files.
!
          WRTforce=.TRUE.
          IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
          NrecADJ(ng)=0
          tADJindx(ng)=0
!
!  NOTE: THE ADM IS FORCED BY dI/dx ONLY when outer=Nouter.
!
!  Time-step adjoint model backwards.
!  ??? What do we do in the case of model error? Save forcing for TLM?
!
          IF (Master) THEN
            WRITE (stdout,20) 'AD', ntstart(ng), ntend(ng)
          END IF

          time(ng)=time(ng)+dt(ng)

          AD_LOOP3 : DO my_iic=ntstart(ng),ntend(ng),-1

            iic(ng)=my_iic
#ifdef SOLVE3D
            CALL ad_main3d (ng)
#else
            CALL ad_main2d (ng)
#endif
            IF (exit_flag.ne.NoError) RETURN

          END DO AD_LOOP3
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps.
!
          CALL ad_wrt_his (ng)
          IF (exit_flag.ne.NoError) RETURN
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
          WRTforce=.FALSE.
          CALL ad_wrt_his (ng)
          IF (exit_flag.ne.NoError) RETURN

#ifdef CONVOLVE
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Convolve adjoint trajectory with model-error covariance and convert
!  to impulse forcing.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
          Nrec=NrecADJ(ng)
          NrecADJ(ng)=0
          tADJindx(ng)=0
          LwrtState2d(ng)=.TRUE.
          LwrtTime(ng)=.FALSE.
          IF (Master) THEN
            WRITE (stdout,30) outer, inner
          END IF
!
!  Clear adjoint state arrays.
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile), SHARED(numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1
              CALL initialize_ocean (ng, TILE, iADM)
            END DO
          END DO
!$OMF END PARALLEL DO
!
!  Convolve initial conditions record (ADJname, record Nrec) with
!  initial conditions background error covariance. Since routine
!  "get_state" loads data into the ghost points, the adjoint
!  solution is read into the tangent linear state arrays by using
!  iTLM instead of iADM in the calling arguments.
!
          ADrec=Nrec
          Lweak=.FALSE.
          CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec, Lold(ng))
          IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays.
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the
!  squared-root adjoint diffusion operator which impose initial
!  conditions background error covaraince. Notice that the spatial
!  convolution is only done for half of the diffusion steps
!  (squared-root filter). Clear tangent linear state arrays when
!  done.
!
          add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1
              CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
# ifdef BALANCE_OPERATOR_NOT_YET
              CALL ad_balance (ng, TILE, Lini, Lold(ng))
# endif
              CALL ad_variability (ng, TILE, Lold(ng), Lweak)
              CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
              CALL initialize_ocean (ng, TILE, iTLM)
              CALL initialize_forces (ng, TILE, iTLM)
            END DO
          END DO
!$OMP END PARALLEL DO

          add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1,+1
              CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng), add)
              CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
              CALL tl_variability (ng, TILE, Lold(ng), Lweak)
# ifdef BALANCE_OPERATOR_NOT_YET
              CALL tl_balance (ng, TILE, Lini, Lold(ng))
# endif
            END DO
          END DO
!$OMP END PARALLEL DO
!
!  Write out tangent linear model initial conditions and tangent
!  linear surface forcing adjustments for next inner
!  loop into ITLname (record Rec1). The tangent model initial
!  conditions are set to the convolved adjoint solution.
!
          CALL tl_wrt_ini (ng, Lold(ng), Rec1)
          IF (exit_flag.ne.NoError) RETURN
!
!  ??? Do we need the adjoint of impulse here???
!
!  If weak constraint, convolve adjoint records in ADJname and impose
!  model error covariance.
!
          IF (Nrec.gt.3) THEN
            LwrtTime(ng)=.TRUE.
            DO rec=1,Nrec-1
              Lweak=.TRUE.
!
!  Read adjoint solution. Since routine "get_state" loads data into the
!  ghost points, the adjoint solution is read in the tangent linear
!  state arrays by using iTLM instead of iADM in the calling arguments.
!
              ADrec=rec
              CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec, Lold(ng))
              IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays.
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the
!  squared-root adjoint diffusion operator which impose the model-error
!  spatial correlations. Notice that the spatial convolution is only
!  done for half of the diffusion steps (squared-root filter). Clear
!  tangent linear state arrays when done.
!
              add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
              DO thread=0,numthreads-1
                subs=NtileX(ng)*NtileE(ng)/numthreads
                DO tile=subs*thread,subs*(thread+1)-1
                  CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
# ifdef BALANCE_OPERATOR_NOT_YET
                  CALL ad_balance (ng, TILE, Lini, Lold(ng))
# endif
                  CALL ad_variability (ng, TILE, Lold(ng), Lweak)
                  CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
                  CALL initialize_ocean (ng, TILE, iTLM)
                  CALL initialize_forces (ng, TILE, iTLM)
                END DO
              END DO
!$OMP END PARALLEL DO
!
!  To insure symmetry, convolve resulting filtered adjoint solution
!  from above with the squared-root (half of steps) tangent linear
!  diffusion operator. Then, multiply result with its corresponding
!  background-error standard deviations.  Since the convolved solution
!  is in the adjoint state arrays, first copy to tangent linear state
!  arrays including the ghosts points. Copy back to adjoint state
!  arrays when done with the convolution for output purposes.
!
              add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
              DO thread=0,numthreads-1
                subs=NtileX(ng)*NtileE(ng)/numthreads
                DO tile=subs*thread,subs*(thread+1)-1,+1
                  CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng), add)
                  CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
                  CALL tl_variability (ng, TILE, Lold(ng), Lweak)
# ifdef BALANCE_OPERATOR_NOT_YET
                  CALL tl_balance (ng, TILE, Lini, Lold(ng))
# endif
                  CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
                END DO
              END DO
!$OMP END PARALLEL DO
!
!  Overwrite ADJname history NetCDF file with convolved adjoint
!  solution.
!
              kstp(ng)=Lold(ng)
# ifdef SOLVE3D
              nstp(ng)=Lold(ng)
# endif
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
            LwrtState2d(ng)=.FALSE.
            LwrtTime(ng)=.TRUE.
          END IF
#endif
!
!  Convert the current adjoint solution in ADJname to impulse forcing.
!  Write out impulse forcing into TLFname NetCDF file. To facilitate
!  the forcing to the TLM and RPM, the forcing is processed and written
!  in increasing time coordinates (recall that the adjoint solution
!  in ADJname is backwards in time).
!
          IF (Master) THEN
            WRITE (stdout,50) outer, inner
          END IF
          tTLFindx(ng)=0
#ifdef DISTRIBUTE
          tile=MyRank
#else
          tile=-1
#endif
!
! AMM: Don't know what to do yet in the weak constraint case.
!
!         CALL wrt_impulse (ng, tile, iADM, ADJname(ng))
          IF (exit_flag.ne.NoError) RETURN
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate tangent linear model forced by the convolved adjoint
!  trajectory.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.TRUE.
          LwrtTLM(ng)=.FALSE.
!
!  Clear tangent linear forcing arrays before entering inner-loop.
!  This is very important since these arrays are non-zero after
!  running the representer model and must be zero when running the
!  tangent linear model.
!
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile) SHARED(numthreads)
          DO thread=0,numthreads-1
#if defined _OPENMP || defined DISTRIBUTE
            subs=NtileX(ng)*NtileE(ng)/numthreads
#else
            subs=1
#endif
            DO tile=subs*thread,subs*(thread+1)-1
              CALL initialize_forces (ng, TILE, iTLM)
            END DO
          END DO
!$OMP END PARALLEL DO
!
!  Set basic state trajectory.
!
          lstr=LEN_TRIM(FWDbase(ng))
          WRITE (FWDname(ng),10) FWDbase(ng)(1:lstr-3), outer-1
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
          IF (FrcRec(ng).gt.3) THEN
            FrequentImpulse=.TRUE.
          END IF
!
!  Initialize tangent linear model from ITLname, record 1.
!
          tITLindx(ng)=Rec1
          CALL tl_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
!
!  Activate the tangent linear relaxation terms if used.
!
          LweakRelax(ng)=.TRUE.
!
!  Run tangent linear model forward and force with convolved adjoint
!  trajectory impulses. Compute (HMBM'H')_n * PSI at observation points
!  which are used in the conjugate gradient algorithm.
!
          IF (Master) THEN
            WRITE (stdout,20) 'TL', ntstart(ng), ntend(ng)
          END IF

          MyTime=time(ng)
          time(ng)=time(ng)-dt(ng)

          TL_LOOP2 : DO my_iic=ntstart(ng),ntend(ng)+1

            iic(ng)=my_iic
#ifdef SOLVE3D
            CALL tl_main3d (ng)
#else
            CALL tl_main2d (ng)
#endif
            MyTime=time(ng)
            IF (exit_flag.ne.NoError) RETURN

          END DO TL_LOOP2
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.FALSE.
!
!  Set basic state trajectory for adjoint inner-loops.
!
          lstr=LEN_TRIM(FWDbase(ng))
          WRITE (FWDname(ng),10) FWDbase(ng)(1:lstr-3), outer-1
!
!  Clear tangent linear forcing arrays before entering inner-loop.
!  This is very important since these arrays are non-zero after
!  running the representer model and must be zero when running the
!  tangent linear model.
!
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile) SHARED(numthreads)
          DO thread=0,numthreads-1
#if defined _OPENMP || defined DISTRIBUTE
            subs=NtileX(ng)*NtileE(ng)/numthreads
#else
            subs=1
#endif
            DO tile=subs*thread,subs*(thread+1)-1
              CALL initialize_forces (ng, TILE, iTLM)
            END DO
          END DO
!$OMP END PARALLEL DO
!
          AD_INNER_LOOP : DO my_inner=Ninner,1,-1
            inner=my_inner

            IF (Master) THEN
              WRITE (stdout,70) 'Adjoint of', uppercase('w4dvar'),      &
     &                          outer, inner
            END IF
!
!  Call adjoint conjugate gradient algorithm.
!
            Lcgini=.FALSE.
            CALL ad_congrad (ng, iTLM, outer, inner, Ninner, Lcgini)
            IF (exit_flag.ne.NoError) RETURN
!
!  Initialize the adjoint model from rest.
!
            LsenPSAS(ng)=.FALSE.
            CALL ad_initial (ng)
            IF (exit_flag.ne.NoError) RETURN
            wrtMisfit(ng)=.FALSE.
!
!  Adjoint of representer relaxation is not applied during the 
!  inner-loops.
!
            LweakRelax(ng)=.FALSE.
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file only once to avoid opening too many files.
!
            WRTforce(ng)=.TRUE.
            IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
            NrecADJ(ng)=0
            tADJindx(ng)=0
!
!  Time-step adjoint model backwards forced with current PSI vector.
!
            IF (Master) THEN
              WRITE (stdout,20) 'AD', ntstart(ng), ntend(ng)
            END IF

            time(ng)=time(ng)+dt(ng)

            AD_LOOP4 : DO my_iic=ntstart(ng),ntend(ng),-1

              iic(ng)=my_iic
#ifdef SOLVE3D
              CALL ad_main3d (ng)
#else
              CALL ad_main2d (ng)
#endif
              IF (exit_flag.ne.NoError) RETURN

            END DO AD_LOOP4
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps
!
            CALL ad_wrt_his (ng)
            IF (exit_flag.ne.NoError) RETURN
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
            WRTforce(ng)=.FALSE.
            CALL ad_wrt_his (ng)
            IF (exit_flag.ne.NoError) RETURN
 
#ifdef CONVOLVE
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Convolve adjoint trajectory with error covariances and convert
!  to impulse forcing.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
            Nrec=NrecADJ(ng)
            NrecADJ(ng)=0
            tADJindx(ng)=0
            LwrtState2d(ng)=.TRUE.
            LwrtTime(ng)=.FALSE.
            IF (Master) THEN
              WRITE (stdout,40) outer, inner
            END IF
!
!  Clear adjoint state arrays.
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile), SHARED(numthreads)
            DO thread=0,numthreads-1
              subs=NtileX(ng)*NtileE(ng)/numthreads
              DO tile=subs*thread,subs*(thread+1)-1
                CALL initialize_ocean (ng, TILE, iADM)
              END DO
            END DO
!$OMF END PARALLEL DO
!
!  Convolve initial conditions record and adjoint forcing
!  (ADJname, record Nrec) with  initial conditions background error 
!  covariance. Note that we only do this for the forcing in 
!  record Nrec since this is the only record for which
!  the adjoing forcing arrays are complete. Since routine
!  "get_state" loads data into the ghost points, the adjoint
!  solution is read into the tangent linear state arrays by using
!  iTLM instead of iADM in the calling arguments.
!
            ADrec=Nrec
            FrcRec(ng)=Nrec
            Lweak=.FALSE.
            CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec, Lold(ng))
            IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays. 
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the 
!  squared-root adjoint diffusion operator which impose initial
!  conditions background error covaraince. Notice that the spatial
!  convolution is only done for half of the diffusion steps
!  (squared-root filter). Clear tangent linear state arrays when
!  done.
!
            add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
            DO thread=0,numthreads-1
              subs=NtileX(ng)*NtileE(ng)/numthreads
              DO tile=subs*thread,subs*(thread+1)-1
                CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
# ifdef BALANCE_OPERATOR_NOT_YET
                CALL ad_balance (ng, TILE, Lini, Lold(ng))
# endif
                CALL ad_variability (ng, TILE, Lold(ng), Lweak)
                CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
                CALL initialize_ocean (ng, TILE, iTLM)
                CALL initialize_forces (ng, TILE, iTLM)
              END DO
            END DO
!$OMP END PARALLEL DO
!
!  To insure symmetry, convolve resulting filtered adjoint solution
!  from above with the squared-root (half of steps) tangent linear
!  diffusion operator. Then, multiply result with its corresponding
!  background-error standard deviations. Since the convolved solution
!  is in the adjoint state arrays, first copy to tangent linear state
!  arrays including the ghosts points.
!
            add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
            DO thread=0,numthreads-1
              subs=NtileX(ng)*NtileE(ng)/numthreads
              DO tile=subs*thread,subs*(thread+1)-1,+1
                CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng), add)
                CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
                CALL tl_variability (ng, TILE, Lold(ng), Lweak)
# ifdef BALANCE_OPERATOR_NOT_YET
                CALL tl_balance (ng, TILE, Lini, Lold(ng))
# endif
              END DO
            END DO
!$OMP END PARALLEL DO
!
!  Write out tangent linear model initial conditions and tangent
!  linear surface forcing adjustments for next inner
!  loop into ITLname (record Rec1). The tangent model initial
!  conditions are set to the convolved adjoint solution.
!
            CALL tl_wrt_ini (ng, Lold(ng), Rec1) 
            IF (exit_flag.ne.NoError) RETURN
!
!  If weak constraint, convolve records 2-Nrec in ADJname and
!  impose model error covariance. NOTE: We will not use the
!  convolved forcing increments generated here since these arrays
!  do not contain the complete solution and are redundant.
!  AMM: We might want to get rid of these unwanted records to
!  avoid any confusion in the future.
!
            IF (Nrec.gt.3) THEN
              LwrtTime(ng)=.TRUE.
              DO rec=1,Nrec-1
                Lweak=.TRUE.
!
!  Read adjoint solution. Since routine "get_state" loads data into the
!  ghost points, the adjoint solution is read in the tangent linear
!  state arrays by using iTLM instead of iADM in the calling arguments.
!
                ADrec=rec
                CALL get_state (ng, iTLM, 4, ADJname(ng), ADrec,        &
     &                          Lold(ng))
                IF (exit_flag.ne.NoError) RETURN
!
!  Load interior solution, read above, into adjoint state arrays. 
!  Then, multiply adjoint solution by the background-error standard
!  deviations. Next, convolve resulting adjoint solution with the 
!  squared-root adjoint diffusion operator which impose the model-error
!  spatial correlations. Notice that the spatial convolution is only
!  done for half of the diffusion steps (squared-root filter). Clear
!  tangent linear state arrays when done.
!
                add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
                DO thread=0,numthreads-1
                  subs=NtileX(ng)*NtileE(ng)/numthreads
                  DO tile=subs*thread,subs*(thread+1)-1
                    CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
# ifdef BALANCE_OPERATOR_NOT_YET
                    CALL ad_balance (ng, TILE, Lini, Lold(ng))
# endif
                    CALL ad_variability (ng, TILE, Lold(ng), Lweak)
                    CALL ad_convolution (ng, TILE, Lold(ng), Lweak, 2)
                    CALL initialize_ocean (ng, TILE, iTLM)
                    CALL initialize_forces (ng, TILE, iTLM)
                  END DO
                END DO
!$OMP END PARALLEL DO
!
!  To insure symmetry, convolve resulting filtered adjoint solution
!  from above with the squared-root (half of steps) tangent linear
!  diffusion operator. Then, multiply result with its corresponding
!  background-error standard deviations.  Since the convolved solution
!  is in the adjoint state arrays, first copy to tangent linear state
!  arrays including the ghosts points. Copy back to adjoint state
!  arrays when done with the convolution for output purposes.
!
                add=.FALSE.
!$OMP PARALLEL DO PRIVATE(ng,thread,subs,tile)                          &
!$OMP&            SHARED(inner,add,numthreads)
                DO thread=0,numthreads-1
                  subs=NtileX(ng)*NtileE(ng)/numthreads
                  DO tile=subs*thread,subs*(thread+1)-1,+1
                    CALL load_ADtoTL (ng, TILE, Lold(ng), Lold(ng), add)
                    CALL tl_convolution (ng, TILE, Lold(ng), Lweak, 2)
                    CALL tl_variability (ng, TILE, Lold(ng), Lweak)
# ifdef BALANCE_OPERATOR_NOT_YET
                    CALL tl_balance (ng, TILE, Lini, Lold(ng))
# endif
                    CALL load_TLtoAD (ng, TILE, Lold(ng), Lold(ng), add)
                  END DO
                END DO
!$OMP END PARALLEL DO
!
!  Overwrite ADJname history NetCDF file with convolved adjoint
!  solution.
!
                kstp(ng)=Lold(ng)
# ifdef SOLVE3D
                nstp(ng)=Lold(ng)
# endif
                CALL ad_wrt_his (ng)
                IF (exit_flag.ne.NoError) RETURN
              END DO
              LwrtState2d(ng)=.FALSE.
              LwrtTime(ng)=.TRUE.
            END IF
#endif
!
!  ??? Do we need the adjoint of impulse here???
!
!  Convert the current adjoint solution in ADJname to impulse forcing.
!  Write out impulse forcing into TLFname NetCDF file. To facilitate
!  the forcing to the TLM and RPM, the forcing is processed and written
!  in increasing time coordinates (recall that the adjoint solution
!  in ADJname is backwards in time).
!
            IF (Master) THEN
              WRITE (stdout,50) outer, inner
            END IF
            tTLFindx(ng)=0
#ifdef DISTRIBUTE
            tile=MyRank
#else
            tile=-1
#endif
!
! AMM: Don't know what to do yet in the weak constraint case.
!
!           CALL wrt_impulse (ng, tile, iADM, ADJname(ng))
            IF (exit_flag.ne.NoError) RETURN
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate tangent linear model.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
            TLMname(ng)=TLMbase(ng)
            wrtNLmod(ng)=.FALSE.
            wrtTLmod(ng)=.TRUE.
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
            IF (FrcRec(ng).gt.3) THEN
              FrequentImpulse=.TRUE.
            END IF
!
!  Initialize tangent linear model from ITLname, record Rec1. 
!
            tITLindx(ng)=Rec1
            CALL tl_initial (ng)
            IF (exit_flag.ne.NoError) RETURN
!
!  Deactivate the tangent linear relaxation terms if used.
!
            LweakRelax(ng)=.FALSE.
!
!  Activate switch to write out initial misfit between model and
!  observations.
!
            IF ((outer.eq.1).and.(inner.eq.1)) THEN
              wrtMisfit(ng)=.TRUE.
            END IF
!
!  Set tangent linear history NetCDF parameters.  Define tangent linear
!  history file at the beggining of each inner loop  to avoid opening
!  too many NetCDF files.
!
            IF (inner.lt.Ninner) LdefTLM(ng)=.FALSE.
            NrecTLM(ng)=0
            tTLMindx(ng)=0
!
!  Run tangent linear model forward and force with convolved adjoint
!  trajectory impulses. Compute R_n * PSI at observation points which
!  are used in the conjugate gradient algorithm.
!
            IF (Master) THEN
              WRITE (stdout,20) 'TL', ntstart(ng), ntend(ng)
            END IF

            MyTime=time(ng)
            time(ng)=time(ng)-dt(ng)

            TL_LOOP3 : DO my_iic=ntstart(ng),ntend(ng)+1

              iic(ng)=my_iic
#ifdef SOLVE3D
              CALL tl_main3d (ng)
#else
              CALL tl_main2d (ng)
#endif
              MyTime=time(ng)

              IF (exit_flag.ne.NoError) RETURN

            END DO TL_LOOP3
            wrtNLmod(ng)=.FALSE.
            wrtTLmod(ng)=.FALSE.

          END DO AD_INNER_LOOP
!
!  Call adjoint conjugate gradient algorithm.
!
          inner=0
          Lcgini=.TRUE.
          CALL ad_congrad (ng, iTLM, outer, inner, Ninner, Lcgini)
!
!  Write out observation sentivity.
!
          IF (outer.eq.1) THEN
            SourceFile='obs_sen_w4dvar.h, ROMS_run'

            CALL netcdf_put_fvar (ng, iTLM, MODname(ng),                &
     &                            Vname(1,idTLmo), ad_ObsVal,           &
     &                            (/1/), (/Mobs/),                      &
     &                            ncid = ncMODid(ng),                   &
     &                            varid = modVid(idTLmo,ng))
            IF (exit_flag.ne.NoError) RETURN

            CALL netcdf_sync (ng, iNLM, MODname(ng), ncMODid(ng))
            IF (exit_flag.ne.NoError) RETURN
          END IF
!
!  Close tangent linear NetCDF file.
!
          SourceFile='obs_sen_w4dvar.h, ROMS_run'

          CALL netcdf_close (ng, iTLM, ncTLMid(ng))
          ncTLMid(ng)=-1
!
!  Close current forward NetCDF file.
!
          SourceFile='obs_sen_w4dvar.h, ROMS_run'

          CALL netcdf_close (ng, iNLM, ncFWDid(ng))
          ncFWDid(ng)=-1

        END DO AD_OUTER_LOOP

      END DO NEST_LOOP
!
 10   FORMAT (a,'_',i3.3,'.nc')
 20   FORMAT (/,1x,a,1x,'ROMS/TOMS: started time-stepping:',            &
     &        '( TimeSteps: ',i8.8,' - ',i8.8,')',/)
 30   FORMAT (' (',i3.3,',',i3.3,'): ',a,' data penalty, Jdata = ',     &
     &        1p,e16.10,0p,t68,a)
 40   FORMAT (/,' Convolving Adjoint Trajectory: Outer = ',i3.3,        &
     &          ' Inner = ',i3.3)
 50   FORMAT (/,' Converting Convolved Adjoint Trajectory to',          &
     &          ' Impulses: Outer = ',i3.3,' Inner = ',i3.3,/)
 60   FORMAT (/,'ROMS/TOMS: Started adjoint Sensitivity calculation',   &
     &          ' ...',/)
 70   FORMAT (/,'ROMS/TOMS: ',a,1x,a,', Outer = ',i3.3,                 &
     &          ' Inner = ',i3.3,/)

      RETURN
      END SUBROUTINE ROMS_run

      SUBROUTINE ROMS_finalize
!
!=======================================================================
!                                                                      !
!  This routine terminates ROMS/TOMS nonlinear, tangent linear, and    !
!  adjoint models execution.                                           !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
      USE mod_iounits
      USE mod_ncparam
      USE mod_scalars
!
!  Local variable declarations.
!
      integer :: ng, thread
!
!-----------------------------------------------------------------------
!  If blowing-up, save latest model state into RESTART NetCDF file.
!-----------------------------------------------------------------------
!
!  If cycling restart records, write solution into record 3.
!
      DO ng=1,Ngrids
        IF (LwrtRST(ng).and.(exit_flag.eq.1)) THEN
          IF (Master) WRITE (stdout,10)
 10       FORMAT (/,' Blowing-up: Saving latest model state into ',     &
     &              ' RESTART file',/)
          IF (LcycleRST(ng).and.(NrecRST(ng).ge.2)) THEN
            tRSTindx(ng)=2
            LcycleRST(ng)=.FALSE.
          END IF
          blowup=exit_flag
          exit_flag=NoError
          CALL wrt_rst (ng)
        END IF
      END DO
!
!-----------------------------------------------------------------------
!  Stop model and time profiling clocks.  Close output NetCDF files.
!-----------------------------------------------------------------------
!
!  Stop time clocks.
!
      IF (Master) THEN
        WRITE (stdout,20)
 20     FORMAT (/,'Elapsed CPU time (seconds):',/)
      END IF

      DO ng=1,Ngrids
!$OMP PARALLEL DO PRIVATE(ng,thread) SHARED(numthreads)
        DO thread=0,numthreads-1
          CALL wclock_off (ng, iNLM, 0)
        END DO
!$OMP END PARALLEL DO
      END DO
!
!  Close IO files.
!
      CALL close_io

      RETURN
      END SUBROUTINE ROMS_finalize

      END MODULE ocean_control_mod
