      SUBROUTINE propagator (ng, Nstr, Nend, state, tl_state)
!
!svn $Id: propagator_fte.h 975 2009-05-05 22:51:13Z kate $
!************************************************** Hernan G. Arango ***
!  Copyright (c) 2002-2009 The ROMS/TOMS Group       Andrew M. Moore   !
!    Licensed under a MIT/X style license                              !
!    See License_ROMS.txt                                              !
!***********************************************************************
!                                                                      !
!  Finite Time Eigenvalues Propagator:                                 !
!                                                                      !
!  This routine it is used to compute the  Finite  Time  Eigenmodes    !
!  (FTEs) of the propagator R(0,t) linearized about a time evolving    !
!  circulation.  It involves a  single integration  of an arbitrary    !
!  perturbation state vector forward in time over the interval [0,t]   !
!  by the tangent linear model.                                        !
!                                                                      !
!   Reference:                                                         !
!                                                                      !
!     Moore, A.M. et al., 2004: A comprehensive ocean prediction and   !
!       analysis system based on the tangent linear and adjoint of a   !
!       regional ocean model, Ocean Modelling, 7, 227-258.             !
!                                                                      !
!***********************************************************************
!
      USE mod_param
      USE mod_parallel
#ifdef SOLVE3D
      USE mod_coupling
#endif
      USE mod_iounits
      USE mod_ocean
      USE mod_scalars
      USE mod_stepping
!
      USE dotproduct_mod, ONLY : tl_statenorm
      USE packing_mod, ONLY : tl_unpack, tl_pack
#ifdef SOLVE3D
      USE set_depth_mod, ONLY: set_depth
#endif
!
!  Imported variable declarations.
!
      integer, intent(in) :: ng, Nstr, Nend

#ifdef ASSUMED_SHAPE
      real(r8), intent(in) :: state(Nstr:)
      real(r8), intent(out) :: tl_state(Nstr:)
#else
      real(r8), intent(in) :: state(Nstr:Nend)
      real(r8), intent(out) :: tl_state(Nstr:Nend)
#endif
!
!  Local variable declarations.
!
#ifdef SOLVE3D
      logical :: FirstPass = .TRUE.
#endif
      integer :: my_iic, subs, tile, thread

      real(r8) :: StateNorm
!
!=======================================================================
!  Forward integration of the tangent linear model.
!=======================================================================
!
      Nrun=Nrun+1
      IF (Master) THEN
        WRITE (stdout,10) ' PROPAGATOR - Iteration Run: ', Nrun,        &
     &                    ',  number converged RITZ values: ', Nconv
      END IF
!
!  Initialize time stepping indices and counters.
!
      iif(ng)=1
      iic(ng)=0
      indx1(ng)=1
      kstp(ng)=1
      krhs(ng)=1
      knew(ng)=1
      PREDICTOR_2D_STEP(ng)=.FALSE.
      synchro_flag(ng)=.TRUE.
!
      nstp(ng)=1
      nrhs(ng)=1
      nnew(ng)=1
!
      tdays(ng)=dstart
      time(ng)=tdays(ng)*day2sec
      ntstart(ng)=INT((time(ng)-dstart*day2sec)/dt(ng))+1
      ntend(ng)=ntimes(ng)
      ntfirst(ng)=ntstart(ng)
!
!-----------------------------------------------------------------------
!  Clear tangent linear state variables. There is not need to clean
!  the basic state arrays since they were zeroth out at initialization
!  and bottom of previous iteration.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile) SHARED(ng,numthreads)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*thread,subs*(thread+1)-1,+1
          CALL initialize_ocean (ng, TILE, iTLM)
        END DO
      END DO
!$OMP END PARALLEL DO

#ifdef SOLVE3D
!
!-----------------------------------------------------------------------
!  Compute basic state initial level thicknesses used for state norm
!  scaling. It uses zero time averaged free-surface (rest state).
!  Therefore, the norm scaling is time invariant.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile)
!$OMP&            SHARED(ng,numthreads)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*(thread+1)-1,subs*thread,-1
          CALL set_depth (ng, TILE)
        END DO
      END DO
!$OMP END PARALLEL DO
#endif
!
!-----------------------------------------------------------------------
!  Unpack tangent linear initial conditions from state vector.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile)                             &
!$OMP&            SHARED(ng,numthreads,Nstr,Nend,state)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*thread,subs*(thread+1)-1,+1
          CALL tl_unpack (ng, TILE, Nstr, Nend, state)
        END DO
      END DO
!$OMP END PARALLEL DO
!
!-----------------------------------------------------------------------
!  Compute initial tangent linear state dot product norm.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile)                             &
!$OMP&            SHARED(ng,numthreads,krhs,nstp,StateNorm)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*(thread+1)-1,subs*thread,-1
          CALL tl_statenorm (ng, TILE, kstp(ng), nstp(ng),              &
     &                       StateNorm)
        END DO
      END DO
!$OMP END PARALLEL DO
      IF (Master) THEN
        WRITE (stdout,20) ' PROPAGATOR - Tangent Initial Norm: ',       &
     &                      StateNorm
      END IF
!
!-----------------------------------------------------------------------
!  Read in initial forcing, climatology and assimilation data from
!  input NetCDF files.  It loads the first relevant data record for
!  the time-interpolation between snapshots.
!-----------------------------------------------------------------------
!
      CALL tl_get_data (ng)
      IF (exit_flag.ne.NoError) RETURN
!
!-----------------------------------------------------------------------
!  Time-step the tangent linear model.
!-----------------------------------------------------------------------
!
      IF (Master) THEN
        WRITE (stdout,30) 'TL', ntstart(ng), ntend(ng)
      END IF

      time(ng)=time(ng)-dt(ng)

      TL_LOOP : DO my_iic=ntstart(ng),ntend(ng)+1

        iic(ng)=my_iic
#ifdef SOLVE3D
        CALL tl_main3d (ng)
#else
        CALL tl_main2d (ng)
#endif
        IF (exit_flag.ne.NoError) RETURN

      END DO TL_LOOP
!
!-----------------------------------------------------------------------
!  Clear nonlinear state (basic state) variables and insure that the
!  time averaged free-surface is zero for scaling below and next
!  iteration.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile) SHARED(ng,numthreads)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*thread,subs*(thread+1)-1,+1
          CALL initialize_ocean (ng, TILE, iNLM)
#ifdef SOLVE3D
          CALL initialize_coupling (ng, TILE, 0)
#endif
        END DO
      END DO
!$OMP END PARALLEL DO

#ifdef SOLVE3D
!
!-----------------------------------------------------------------------
!  Compute basic state final level thicknesses used for state norm
!  scaling. It uses zero time averaged free-surface (rest state).
!  Therefore, the norm scaling is time invariant.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile)
!$OMP&            SHARED(ng,numthreads)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*(thread+1)-1,subs*thread,-1
          CALL set_depth (ng, TILE)
        END DO
      END DO
!$OMP END PARALLEL DO
#endif
!
!-----------------------------------------------------------------------
!  Compute final tangent linear state dot product norm.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile)                             &
!$OMP&            SHARED(ng,numthreads,krhs,nstp,StateNorm)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*thread,subs*(thread+1)-1,+1
          CALL tl_statenorm (ng, TILE, knew(ng), nstp(ng), StateNorm)
        END DO
      END DO
!$OMP END PARALLEL DO
      IF (Master) THEN
        WRITE (stdout,20) ' PROPAGATOR - Tangent   Final Norm: ',       &
     &                    StateNorm
      END IF
!
!-----------------------------------------------------------------------
!  Pack final tangent linear solution into tangent state vector.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile)                             &
!$OMP&            SHARED(ng,numthreads,Nstr,Nend,tl_state)
      DO thread=0,numthreads-1
        subs=NtileX(ng)*NtileE(ng)/numthreads
        DO tile=subs*thread,subs*(thread+1)-1
          CALL tl_pack (ng, TILE, Nstr, Nend, tl_state)
        END DO
      END DO
!$OMP END PARALLEL DO
!

 10   FORMAT (/,a,i3,a,i3/)
 20   FORMAT (/,a,1p,e15.6,/)
 30   FORMAT (/,1x,a,1x,'ROMS/TOMS: started time-stepping:',            &
     &        '( TimeSteps: ',i8.8,' - ',i8.8,')',/)

      RETURN
      END SUBROUTINE propagator
