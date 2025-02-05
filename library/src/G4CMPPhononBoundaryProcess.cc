/***********************************************************************\
 * This software is licensed under the terms of the GNU General Public *
 * License version 3 or later. See G4CMP/LICENSE for the full license. *
\***********************************************************************/

/// \file library/src/G4PhononReflection.cc
/// \brief Implementation of the G4PhononReflection class
//
// This process handles the interaction of phonons with
// boundaries. Implementation of this class is highly
// geometry dependent.Currently, phonons are killed when
// they reach a boundary. If the other side of the
// boundary was Al, a hit is registered.
//
// $Id$
//
// 20131115  Throw exception if track's polarization state is invalid.
// 20140103  Move charge version of code here, still commented out
// 20140331  Add required process subtype code
// 20160624  Use GetTrackInfo() accessor
// 20160903  Migrate to use G4CMPBoundaryUtils for most functionality
// 20160906  Follow constness of G4CMPBoundaryUtils
// 20161114  Use new G4CMPPhononTrackInfo
// 20170829  Add detailed diagnostics to identify boundary issues
// 20170928  Replace "pol" with "mode" for phonons
// 20181010  J. Singh -- Use new G4CMPAnharmonicDecay for boundary decays
// 20181011  M. Kelsey -- Add LoadDataForTrack() to initialize decay utility.
// 20220712  M. Kelsey -- Pass process pointer to G4CMPAnharmonicDecay
// 20220905  G4CMP-310 -- Add increments of kPerp to avoid bad reflections.
// 20220910  G4CMP-299 -- Use fabs(k) in absorption test.
// 20240718  G4CMP-317 -- Initial implementation of surface displacement.
// 20250124  G4CMP-447 -- Use FillParticleChange() to update wavevector and Vg.
// 20250204  G4CMP-459 -- Handle edge cases during surface displacement.

#include "G4CMPPhononBoundaryProcess.hh"
#include "G4CMPAnharmonicDecay.hh"
#include "G4CMPConfigManager.hh"
#include "G4CMPGeometryUtils.hh"
#include "G4CMPPhononTrackInfo.hh"
#include "G4CMPSurfaceProperty.hh"
#include "G4CMPTrackUtils.hh"
#include "G4CMPUtils.hh"
#include "G4ExceptionSeverity.hh"
#include "G4LatticePhysical.hh"
#include "G4ParallelWorldProcess.hh"
#include "G4ParticleChange.hh"
#include "G4PhononPolarization.hh"
#include "G4PhysicalConstants.hh"
#include "G4RunManager.hh"
#include "G4Step.hh"
#include "G4StepPoint.hh"
#include "G4SystemOfUnits.hh"
#include "G4ThreeVector.hh"
#include "G4Track.hh"
#include "G4VParticleChange.hh"
#include "G4VSolid.hh"
#include "Randomize.hh"


// Constructor and destructor

G4CMPPhononBoundaryProcess::G4CMPPhononBoundaryProcess(const G4String& aName)
  : G4VPhononProcess(aName, fPhononReflection), G4CMPBoundaryUtils(this),
    anharmonicDecay(new G4CMPAnharmonicDecay(this)) {;}

G4CMPPhononBoundaryProcess::~G4CMPPhononBoundaryProcess() {
  delete anharmonicDecay;
}


// Configure for current track including AnharmonicDecay utility

void G4CMPPhononBoundaryProcess::LoadDataForTrack(const G4Track* track) {
  G4CMPProcessUtils::LoadDataForTrack(track);
  anharmonicDecay->LoadDataForTrack(track);
}


// Compute and return step length

G4double G4CMPPhononBoundaryProcess::
PostStepGetPhysicalInteractionLength(const G4Track& aTrack,
                                     G4double previousStepSize,
                                     G4ForceCondition* condition) {
  return GetMeanFreePath(aTrack, previousStepSize, condition);
}

G4double G4CMPPhononBoundaryProcess::GetMeanFreePath(const G4Track& /*aTrack*/,
                                             G4double /*prevStepLength*/,
                                             G4ForceCondition* condition) {
  *condition = Forced;
  return DBL_MAX;
}


// Process action

G4VParticleChange*
G4CMPPhononBoundaryProcess::PostStepDoIt(const G4Track& aTrack,
                                         const G4Step& aStep) {
  // NOTE:  G4VProcess::SetVerboseLevel is not virtual!  Can't overlaod it
  G4CMPBoundaryUtils::SetVerboseLevel(verboseLevel);

  aParticleChange.Initialize(aTrack);
  if (!IsGoodBoundary(aStep))
    return G4VDiscreteProcess::PostStepDoIt(aTrack, aStep);

  if (verboseLevel>1) {
    G4cout << GetProcessName() << "::PostStepDoIt "
           << "Event " << G4RunManager::GetRunManager()->GetCurrentEvent()->GetEventID()
           << " Track " << aTrack.GetTrackID() << " Step " << aTrack.GetCurrentStepNumber()
           << G4endl;
  }

  if (verboseLevel>2) {
    G4cout << " K direction: " << GetLocalWaveVector(aTrack).unit()
           << "\n P direction: " << aTrack.GetMomentumDirection() << G4endl;
  }

  ApplyBoundaryAction(aTrack, aStep, aParticleChange);

  ClearNumberOfInteractionLengthLeft();		// All processes should do this!
  return &aParticleChange;
}


G4bool G4CMPPhononBoundaryProcess::AbsorbTrack(const G4Track& aTrack,
                                               const G4Step& aStep) const {
  G4double absMinK = GetMaterialProperty("absMinK");
  G4ThreeVector k = G4CMP::GetTrackInfo<G4CMPPhononTrackInfo>(aTrack)->k();

  if (verboseLevel>1) {
    G4cout << GetProcessName() << "::AbsorbTrack() k " << k
	   << "\n |k_perp| " << fabs(k*G4CMP::GetSurfaceNormal(aStep))
	   << " vs. absMinK " << absMinK << G4endl;
  }

  return (G4CMPBoundaryUtils::AbsorbTrack(aTrack,aStep) &&
	  fabs(k*G4CMP::GetSurfaceNormal(aStep)) > absMinK);
}


void G4CMPPhononBoundaryProcess::
DoReflection(const G4Track& aTrack, const G4Step& aStep,
	     G4ParticleChange& particleChange) {
  auto trackInfo = G4CMP::GetTrackInfo<G4CMPPhononTrackInfo>(aTrack);

  if (verboseLevel>1) {
    G4cout << GetProcessName() << ": Track reflected "
           << trackInfo->ReflectionCount() << " times." << G4endl;
  }

  G4ThreeVector waveVector = trackInfo->k();
  G4int mode = GetPolarization(aStep.GetTrack());
  G4ThreeVector surfNorm = G4CMP::GetSurfaceNormal(aStep);

  if (verboseLevel>2) {
    G4cout << "\n Old wavevector direction " << waveVector.unit()
	   << "\n Old momentum direction   " << aTrack.GetMomentumDirection()
	   << G4endl;
  }

  // Check whether step has proper boundary-stopped geometry
  G4ThreeVector surfacePoint;
  if (!CheckStepBoundary(aStep, surfacePoint)) {
    if (verboseLevel>2)
      G4cout << " Boundary point moved to " << surfacePoint << G4endl;

    particleChange.ProposePosition(surfacePoint);	// IS THIS CORRECT?!?
  }

  G4double freq = GetKineticEnergy(aTrack)/h_Planck;	// E = hf, f = E/h
  G4double specProb = surfProp->SpecularReflProb(freq);
  G4double diffuseProb = surfProp->DiffuseReflProb(freq);
  G4double downconversionProb = surfProp->AnharmonicReflProb(freq);

  // Empirical functions may lead to non normalised probabilities.
  // Normalise here.

  G4double norm = specProb + diffuseProb + downconversionProb;

  specProb /= norm;
  diffuseProb /= norm;
  downconversionProb /= norm;

  G4ThreeVector reflectedKDir;

  G4double random = G4UniformRand();

  if (verboseLevel > 2) {
    G4cout << "Surface Downconversion Probability: " << downconversionProb
	   << " random: " << random << G4endl;
  }

  G4String refltype = "";		// For use in failure message if needed

  if (random < downconversionProb) {
    if (verboseLevel > 2) G4cout << " Anharmonic Decay at boundary." << G4endl;

    /* Do Downconversion */
    anharmonicDecay->DoDecay(aTrack, aStep, particleChange);
    G4Track* sec1 = particleChange.GetSecondary(0);
    G4Track* sec2 = particleChange.GetSecondary(1);

    G4ThreeVector vec1 = GetLambertianVector(surfNorm, mode);
    G4ThreeVector vec2 = GetLambertianVector(surfNorm, mode);

    sec1->SetMomentumDirection(vec1);
    sec2->SetMomentumDirection(vec2);

    return;
  } else if (random < downconversionProb + specProb) {
    reflectedKDir = GetReflectedVector(waveVector, surfNorm, mode, surfacePoint);
    refltype = "specular";
  } else {
    reflectedKDir = GetLambertianVector(surfNorm, mode);
    refltype = "diffuse";
  }

  // Update trackInfo wavevector and particleChange's group velocity and momentum direction
  // reflectedKDir is in global coordinates here - no conversion needed
  FillParticleChange(particleChange, aTrack, reflectedKDir);

  G4ThreeVector vdir = *particleChange.GetMomentumDirection();

  if (verboseLevel>2) {
    G4cout << "\n New wavevector direction " << reflectedKDir
	   << "\n New momentum direction   " << vdir << G4endl;
  }

  // If reflection failed, report problem and kill the track
  if (!G4CMP::PhononVelocityIsInward(theLattice,mode,reflectedKDir,surfNorm)) {
    G4Exception((GetProcessName()+"::DoReflection").c_str(), "Boundary010",
		JustWarning, ("Phonon "+refltype+" reflection failed"+"\nPhonon mode at time of death: "+G4PhononPolarization::Label(mode)).c_str());
    DoSimpleKill(aTrack, aStep, aParticleChange);
    return;
  }

  // SANITY CHECK:  Project a 1 um step in the new direction, see if it
  // is still in the correct (pre-step) volume.

  if (verboseLevel>2) {
    G4ThreeVector stepPos = surfacePoint + 1*um * vdir;

    G4cout << " New travel direction " << vdir
	   << "\n from " << surfacePoint << "\n   to " << stepPos << G4endl;

    G4ThreeVector stepLocal = GetLocalPosition(stepPos);
    G4VSolid* solid = aStep.GetPreStepPoint()->GetPhysicalVolume()->GetLogicalVolume()->GetSolid();

    EInside place = solid->Inside(stepLocal);
    G4cout << " After trial step, " << (place==kInside ? "inside"
					: place==kOutside ? "OUTSIDE"
					: "on surface") << G4endl;
  }
}


// Generate specular reflection corrected for momentum dispersion

G4ThreeVector G4CMPPhononBoundaryProcess::
GetReflectedVector(const G4ThreeVector& waveVector,
		   const G4ThreeVector& surfNorm, G4int mode,
		   const G4ThreeVector& surfacePoint) const {
  // Specular reflecton should reverse momentum along normal
  G4ThreeVector reflectedKDir = waveVector.unit();	// Unit vector of initial k-vector (before reflection)
  G4double kPerp = reflectedKDir * surfNorm;		// Dot product between k and norm. Must be >= 0 at this stage
  (reflectedKDir -= 2.*kPerp*surfNorm).setMag(1.);	// Law of reflections. reflectedKDir is now inward
   
  if (G4CMP::PhononVelocityIsInward(theLattice,mode,reflectedKDir,surfNorm))
    return reflectedKDir;

  // Reflection didn't work as expected, need to correct   

  // If the reflected wave vector cannot propagate in the bulk
  // (i.e., the reflected k⃗ has an associated v⃗g which is not inwardly directed.)
  // That surface wave will propagate until it reaches a point
  // where the wave vector has an inwardly directed v⃗g.
  RotateToLocalDirection(reflectedKDir);	// Put reflectedKDir in local frame
  G4ThreeVector newNorm = surfNorm;		// Initial normal at point of reflection
  RotateToLocalDirection(newNorm);		// Rotate norm to local frame
  
  G4ThreeVector stepLocalPos = GetLocalPosition(surfacePoint);			// Get local coor on surface
  G4VSolid* solid = GetCurrentVolume()->GetLogicalVolume()->GetSolid();		// Obtain detector solid object
  G4ThreeVector oldNorm = newNorm;						// Save previous norm for debugging
  G4double surfAdjust = solid->DistanceToIn(stepLocalPos, -newNorm);		// Find the distance from point to surface along norm (- means inward)
  G4double kPerpMag = reflectedKDir.dot(newNorm);				// Must be <=0; reflectedKDir is inward and norm is outward

  G4ThreeVector kPerpV = kPerpMag * newNorm;		// Get perpendicular component of reflected k (negative implied in kPerpMag for inward pointing)
  G4ThreeVector kTan = reflectedKDir - kPerpV;		// Get kTan: reflectedKDir = kPerpV + kTan
  G4ThreeVector axis = kPerpV.cross(kTan).unit();	// Get axis prep to both kTan and kPerpV to rotate about
  G4double phi = 0.;

  const G4double stepSize = 1.*um;	// Distance to step each trial

  const G4int maxAttempts = 1000;
  G4int nAttempts = 0;

  // debugging only DELETE
  G4ThreeVector oldkTan = kTan;
  G4ThreeVector oldkPerpV = kPerpV;
  G4ThreeVector oldstepLocalPos = stepLocalPos;

  // FIXME: Need defined units
  if (verboseLevel>3) {
    G4cout << "GetReflectedVector:beforeLoop -> "
      << ", stepLocalPos = " << stepLocalPos
      << ", kPerpMag (newNorm dot reflectedKDir) = " << kPerpMag
      << ", newNorm = " << newNorm
      << ", reflectedKDir = " << reflectedKDir
      << ", kPerpV (kPerpMag * newNorm) = " << kPerpV
      << ", kTan (reflectedKDir - kPerpV) = " << kTan << G4endl;
  }

  // Assumes everything is in Global. Just add the GetGlobal in the loop conditions.
  while (!G4CMP::PhononVelocityIsInward(theLattice, mode,
   GetGlobalDirection(reflectedKDir), GetGlobalDirection(newNorm))
	 && nAttempts++ < maxAttempts) {
    // Step along the surface in the tangential direction of k (or v_g)
    stepLocalPos += stepSize * kTan.unit();	// Step along kTan direction - this point is now outside the detector

    // Get the local normal at the new surface point
    oldNorm = newNorm;					// Save normal at old position
    newNorm = solid->SurfaceNormal(stepLocalPos);	// Get new normal at new position

    // debugging only DELETE
    oldstepLocalPos = stepLocalPos;			// Save old position on detector

    // FIXME: Find point on surface nearest to stepLocalPos, and reset
    surfAdjust = solid->DistanceToIn(stepLocalPos, -newNorm);	// Get distance along normal from new position back to detector surface

    // If surfAdjust > 1, we stepped off an edge and need to correct
    if (surfAdjust > 1) {
      stepLocalPos = GetEdgePosition(stepLocalPos, reflectedKDir);
      reflectedKDir = GetReflectionOnEdge(stepLocalPos, reflectedKDir);
    } else {
      // Adjust position to be back on detector surface
      stepLocalPos -= surfAdjust * newNorm;
    }

    // Get rotation axis perpendicular to waveVector-normal plane
    axis = kPerpV.cross(kTan).unit();

    // debugging only DELETE
    oldkTan = kTan;
    oldkPerpV = kPerpV;

    // Get new kPerpV (newNorm * kPerpMag)
    kPerpV = kPerpMag * newNorm;	// Get perpendicular component of reflected k w/ new norm (negative implied in kPerpMag for inward pointing)

    // Rotate kTan to be perpendicular to new normal
    phi = oldNorm.azimAngle(newNorm, axis);	// Angle bewteen oldNorm and newNorm
    kTan = kTan.rotate(axis, phi);		// Rotate kTan by the angular distance between oldNorm and newNorm

    // Calculate new reflectedKDir (kTan + kPerpV)
    reflectedKDir = kTan + kPerpV;

    // Debugging: Can be removed?
    G4ThreeVector vDir = theLattice->MapKtoVDir(mode, reflectedKDir);

    // FIXME: Need defined units
    if (verboseLevel>3) {
      G4cout << " "
       << "GetReflectedVector:insideLoop -> "
       << "attempts = " << nAttempts
       << ", oldstepLocalPos = " << oldstepLocalPos
       << ", surfAdjust = " << surfAdjust
       << ", stepLocalPos = " << stepLocalPos
       << ", axis (oldkPerpV cross oldkTan).unit() = " << axis
       << ", oldkPerpV = " << oldkPerpV
       << ", oldkTan = " << oldkTan
       << ", kPerpV (kPerpMag * newNorm) = " << kPerpV
       << ", kPerpMag = " << kPerpMag
       << ", newNorm = " << newNorm
       << ", phi (oldNorm azimAngle (newNorm, axis)) = " << phi
       << ", oldNorm = " << oldNorm
       << ", kTan (rotate by phi about axis) = " << kTan
       << ", reflectedKDir (kTan + kPerpV) = " << reflectedKDir
       << ", Phonon mode = " << G4PhononPolarization::Label(mode)
       << ", New group velocity: " << vDir << G4endl;
    }
  }

  // Restore global coordinates to return result for processing
  RotateToGlobalDirection(reflectedKDir);
  RotateToGlobalPosition(stepLocalPos);

  if (verboseLevel>2) {
    if (!G4CMP::PhononVelocityIsInward(theLattice,mode,reflectedKDir, newNorm)) {
      G4cout << "GetReflectedVector:afterLoop -> Phonon displacement failed after " << nAttempts - 1 << " attempts.";
    }
    else
    {
      G4cout << "GetReflectedVector:afterLoop -> "
       << "attempts = " << nAttempts
       << ", waveVector = " << waveVector
       << ", reflectedKDir = " << reflectedKDir
       << ", initialGlobalPostion = " << surfacePoint
       << ", finalGlobalPosition = " << stepLocalPos << G4endl;
    }
  }

  return reflectedKDir;
}


// Generate diffuse reflection according to 1/cos distribution

G4ThreeVector G4CMPPhononBoundaryProcess::
GetLambertianVector(const G4ThreeVector& surfNorm, G4int mode) const {
  G4ThreeVector reflectedKDir;
  const G4int maxTries = 1000;
  G4int nTries = 0;
  do {
    reflectedKDir = G4CMP::LambertReflection(surfNorm);
  } while (nTries++ < maxTries &&
	   !G4CMP::PhononVelocityIsInward(theLattice, mode,
					  reflectedKDir, surfNorm));

  return reflectedKDir;
}


// Get the position on the edge of two surfaces

G4ThreeVector G4CMPPhononBoundaryProcess::
GetEdgePosition(const G4ThreeVector& stepLocalPos, const G4ThreeVector& waveVector) const {
  // Get normal at current position
  G4ThreeVector currNorm = solid->SurfaceNormal(stepLocalPos);

  // Get tangential component of wavevector
  G4double kPerpMag = waveVector.dot(currNorm);
  G4ThreeVector kPerp = kPerpMag * currNorm;
  G4ThreeVector kTan = waveVector - kPerpV;

  // Step into the normal to get comfortably on the other surface
  G4ThreeVector edgePos = stepLocalPos - 1.*mm * currNorm;

  // Step back to surface along kTan
  G4double surfAdjust = solid->DistanceToIn(edgePos, -kTan);
  edgePos -= surfAdjust * kTan;
  edgePos += 1.*mm * currNorm;

  return edgePos;
}


// Reflect "surface mode" phonon at the edge cases

G4ThreeVector G4CMPPhononBoundaryProcess::
GetReflectionOnEdge(const G4ThreeVector& stepLocalPos, const G4ThreeVector& waveVector) const {
  // Get normal at current position
  G4ThreeVector currNorm = solid->SurfaceNormal(stepLocalPos);

  // Get bordering surface's normal
  G4ThreeVector edgePos = stepLocalPos - 1.*mm * currNorm;
  G4ThreeVector newNorm = solid->SurfaceNormal(edgePos);

  // Reflect vector against new normal
  G4double kPerp = waveVector * newNorm;
  G4ThreeVector reflectedKDir = (waveVector - 2.*kPerp*newNorm).setMag(1.);

  return reflectedKDir;
}