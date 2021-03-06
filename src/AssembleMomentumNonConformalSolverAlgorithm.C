/*------------------------------------------------------------------------*/
/*  Copyright 2014 Sandia Corporation.                                    */
/*  This software is released under the license detailed                  */
/*  in the file, LICENSE, which is located in the top-level Nalu          */
/*  directory structure                                                   */
/*------------------------------------------------------------------------*/


// nalu
#include <AssembleMomentumNonConformalSolverAlgorithm.h>
#include <EquationSystem.h>
#include <DgInfo.h>
#include <FieldTypeDef.h>
#include <LinearSystem.h>
#include <NonConformalInfo.h>
#include <NonConformalManager.h>
#include <Realm.h>
#include <SolutionOptions.h>
#include <TimeIntegrator.h>
#include <master_element/MasterElement.h>

// stk_mesh/base/fem
#include <stk_mesh/base/BulkData.hpp>
#include <stk_mesh/base/Field.hpp>
#include <stk_mesh/base/FieldParallel.hpp>
#include <stk_mesh/base/MetaData.hpp>
#include <stk_mesh/base/Part.hpp>

namespace sierra{
namespace nalu{

//==========================================================================
// Class Definition
//==========================================================================
// AssembleMomentumNonConformalSolverAlgorithm - lhs for NC bc (DG)
//                                               used for both edge
//                                               and element; WIP..
//==========================================================================
//--------------------------------------------------------------------------
//-------- constructor -----------------------------------------------------
//--------------------------------------------------------------------------
AssembleMomentumNonConformalSolverAlgorithm::AssembleMomentumNonConformalSolverAlgorithm(
  Realm &realm,
  stk::mesh::Part *part,
  EquationSystem *eqSystem,
  VectorFieldType *velocity,
  ScalarFieldType *diffFluxCoeff)
  : SolverAlgorithm(realm, part, eqSystem),
    velocity_(velocity),
    diffFluxCoeff_(diffFluxCoeff),
    coordinates_(NULL),
    exposedAreaVec_(NULL),
    ncMassFlowRate_(NULL),
    robinStyle_(false),
    dsFactor_(1.0),
    upwindAdvection_(realm_.get_nc_alg_upwind_advection()),
    includeDivU_(realm_.get_divU())
{
  // save off fields
  stk::mesh::MetaData & meta_data = realm_.meta_data();
  coordinates_ = meta_data.get_field<VectorFieldType>(stk::topology::NODE_RANK, realm_.get_coordinates_name());
  exposedAreaVec_ = meta_data.get_field<GenericFieldType>(meta_data.side_rank(), "exposed_area_vector");  
  ncMassFlowRate_ = meta_data.get_field<GenericFieldType>(meta_data.side_rank(), "nc_mass_flow_rate");

  // what do we need ghosted for this alg to work?
  ghostFieldVec_.push_back(&(velocity_->field_of_state(stk::mesh::StateNP1)));
  ghostFieldVec_.push_back(diffFluxCoeff_);
  ghostFieldVec_.push_back(coordinates_);
  
  // specific algorithm options
  NonConformalAlgType algType = realm_.get_nc_alg_type();
  switch ( algType ) {
    case NC_ALG_TYPE_DG:
      dsFactor_ = 1.0;
      robinStyle_ = false;
      break;
      
    case NC_ALG_TYPE_DS: 
      dsFactor_ = 0.0;
      // robinStyle_ does not matter here..
      break;
    
    case NC_ALG_TYPE_RB:
      dsFactor_ = 1.0;
      robinStyle_ = true;
      
    default:
      // nothing to do... parsing should have caught this...
      break;
  }

  NaluEnv::self().naluOutputP0() << "NC Momentum options: dsFactor/robinStyle/upwind: " 
                                 << dsFactor_ << " " << robinStyle_ << " " << upwindAdvection_ << std::endl;
  
}

//--------------------------------------------------------------------------
//-------- initialize_connectivity -----------------------------------------
//--------------------------------------------------------------------------
void
AssembleMomentumNonConformalSolverAlgorithm::initialize_connectivity()
{
  eqSystem_->linsys_->buildNonConformalNodeGraph(partVec_);
}

//--------------------------------------------------------------------------
//-------- execute ---------------------------------------------------------
//--------------------------------------------------------------------------
void
AssembleMomentumNonConformalSolverAlgorithm::execute()
{

  stk::mesh::BulkData & bulk_data = realm_.bulk_data();
  stk::mesh::MetaData & meta_data = realm_.meta_data();

  const int nDim = meta_data.spatial_dimension();

  // space for LHS/RHS; nodesPerElem*nodesPerElem and nodesPerElem
  std::vector<double> lhs;
  std::vector<double> rhs;
  std::vector<stk::mesh::Entity> connected_nodes;
 
  // ip values; both boundary and opposing surface
  std::vector<double> currentIsoParCoords(nDim);
  std::vector<double> opposingIsoParCoords(nDim);
  std::vector<double> cNx(nDim);
  std::vector<double> oNx(nDim);

  // mapping for -1:1 -> -0.5:0.5 volume element
  std::vector<double> currentElementIsoParCoords(nDim);
  std::vector<double> opposingElementIsoParCoords(nDim);

  // c/o velocity and normal flux
  std::vector<double> currentUBip(nDim);
  std::vector<double> opposingUBip(nDim);
 
  // interpolate nodal values to point-in-elem
  const int sizeOfScalarField = 1;
  const int sizeOfVectorField = nDim;
 
  // pointers to fixed values
  double *p_cNx = &cNx[0];
  double *p_oNx = &oNx[0];

  // nodal fields to gather
  std::vector<double> ws_c_face_velocity;
  std::vector<double> ws_o_face_velocity;
  std::vector<double> ws_c_elem_velocity;
  std::vector<double> ws_o_elem_velocity;
  std::vector<double> ws_c_elem_coordinates;
  std::vector<double> ws_o_elem_coordinates;
  std::vector<double> ws_c_diffFluxCoeff;
  std::vector<double> ws_o_diffFluxCoeff;  

  // master element data
  std::vector<double> ws_c_dndx;
  std::vector<double> ws_o_dndx;
  std::vector<double> ws_c_det_j;
  std::vector<double> ws_o_det_j;
  std::vector <double > ws_c_general_shape_function;
  std::vector <double > ws_o_general_shape_function;
  std::vector<int> ws_c_face_node_ordinals;
  std::vector<int> ws_o_face_node_ordinals;

  // deal with state
  VectorFieldType &velocityNp1 = velocity_->field_of_state(stk::mesh::StateNP1);

  // parallel communicate ghosted entities
  if ( NULL != realm_.nonConformalManager_->nonConformalGhosting_ )
    stk::mesh::communicate_field_data(*(realm_.nonConformalManager_->nonConformalGhosting_), ghostFieldVec_);

  // iterate nonConformalManager's dgInfoVec
  std::vector<NonConformalInfo *>::iterator ii;
  for( ii=realm_.nonConformalManager_->nonConformalInfoVec_.begin();
       ii!=realm_.nonConformalManager_->nonConformalInfoVec_.end(); ++ii ) {

    // extract vector of DgInfo
    std::vector<std::vector<DgInfo *> > &dgInfoVec = (*ii)->dgInfoVec_;
    
    std::vector<std::vector<DgInfo*> >::iterator idg;
    for( idg=dgInfoVec.begin(); idg!=dgInfoVec.end(); ++idg ) {

      std::vector<DgInfo *> &faceDgInfoVec = (*idg);

      // now loop over all the DgInfo objects on this particular exposed face
      for ( size_t k = 0; k < faceDgInfoVec.size(); ++k ) {

        DgInfo *dgInfo = faceDgInfoVec[k];
      
        // extract current/opposing face/element
        stk::mesh::Entity currentFace = dgInfo->currentFace_;
        stk::mesh::Entity opposingFace = dgInfo->opposingFace_;
        stk::mesh::Entity currentElement = dgInfo->currentElement_;
        stk::mesh::Entity opposingElement = dgInfo->opposingElement_;
        stk::topology currentElementTopo = dgInfo->currentElementTopo_;
        stk::topology opposingElementTopo = dgInfo->opposingElementTopo_;
        const int currentFaceOrdinal = dgInfo->currentFaceOrdinal_;
        const int opposingFaceOrdinal = dgInfo->opposingFaceOrdinal_;
        
        // master element; face and volume
        MasterElement * meFCCurrent = dgInfo->meFCCurrent_; 
        MasterElement * meFCOpposing = dgInfo->meFCOpposing_;
        MasterElement * meSCSCurrent = dgInfo->meSCSCurrent_; 
        MasterElement * meSCSOpposing = dgInfo->meSCSOpposing_;
        
        // local ip, ordinals, etc
        const int currentGaussPointId = dgInfo->currentGaussPointId_;
        currentIsoParCoords = dgInfo->currentIsoParCoords_;
        opposingIsoParCoords = dgInfo->opposingIsoParCoords_;
        
        // extract some master element info
        const int currentNodesPerFace = meFCCurrent->nodesPerElement_;
        const int opposingNodesPerFace = meFCOpposing->nodesPerElement_;
        const int currentNodesPerElement = meSCSCurrent->nodesPerElement_;
        const int opposingNodesPerElement = meSCSOpposing->nodesPerElement_;
  
        // resize some things; matrix related
        const int lhsSize = (currentNodesPerFace+opposingNodesPerFace)*nDim*(currentNodesPerFace+opposingNodesPerFace)*nDim;
        const int rhsSize = (currentNodesPerFace+opposingNodesPerFace)*nDim;
        lhs.resize(lhsSize);
        rhs.resize(rhsSize);
        connected_nodes.resize(currentNodesPerFace+opposingNodesPerFace);
        
        // algorithm related; element; dndx will be at a single gauss point...
        ws_c_elem_velocity.resize(currentNodesPerElement*nDim);
        ws_o_elem_velocity.resize(opposingNodesPerElement*nDim);
        ws_c_elem_coordinates.resize(currentNodesPerElement*nDim);
        ws_o_elem_coordinates.resize(opposingNodesPerElement*nDim);
        ws_c_dndx.resize(nDim*currentNodesPerElement);
        ws_o_dndx.resize(nDim*opposingNodesPerElement);
        ws_c_det_j.resize(1);
        ws_o_det_j.resize(1);

        // algorithm related; face
        ws_c_face_velocity.resize(currentNodesPerFace*nDim);
        ws_o_face_velocity.resize(opposingNodesPerFace*nDim);
        ws_c_diffFluxCoeff.resize(currentNodesPerFace);
        ws_o_diffFluxCoeff.resize(opposingNodesPerFace);
        ws_c_general_shape_function.resize(currentNodesPerFace);
        ws_o_general_shape_function.resize(opposingNodesPerFace);
        
        // face node identification
        ws_c_face_node_ordinals.resize(currentNodesPerFace);
        ws_o_face_node_ordinals.resize(opposingNodesPerFace);

        // pointers
        double *p_lhs = &lhs[0];
        double *p_rhs = &rhs[0];
        
        double *p_c_face_velocity = &ws_c_face_velocity[0];
        double *p_o_face_velocity = &ws_o_face_velocity[0];
        double *p_c_elem_velocity = &ws_c_elem_velocity[0];
        double *p_o_elem_velocity = &ws_o_elem_velocity[0];
        double *p_c_elem_coordinates = &ws_c_elem_coordinates[0];
        double *p_o_elem_coordinates = &ws_o_elem_coordinates[0];
        double *p_c_diffFluxCoeff = &ws_c_diffFluxCoeff[0];
        double *p_o_diffFluxCoeff = &ws_o_diffFluxCoeff[0];
               
        // me pointers
        double *p_c_general_shape_function = &ws_c_general_shape_function[0];
        double *p_o_general_shape_function = &ws_o_general_shape_function[0];
        double *p_c_dndx = &ws_c_dndx[0];
        double *p_o_dndx = &ws_o_dndx[0];
   
        // populate current face_node_ordinals
        currentElementTopo.side_node_ordinals(currentFaceOrdinal, ws_c_face_node_ordinals.begin());

        // gather current face data; sneak in first of connected nodes
        stk::mesh::Entity const* current_face_node_rels = bulk_data.begin_nodes(currentFace);
        const int current_num_face_nodes = bulk_data.num_nodes(currentFace);
        for ( int ni = 0; ni < current_num_face_nodes; ++ni ) {
          stk::mesh::Entity node = current_face_node_rels[ni];
          // set connected nodes
          connected_nodes[ni] = node;
          // gather; scalar
          p_c_diffFluxCoeff[ni] = *stk::mesh::field_data(*diffFluxCoeff_, node);
          // gather; vector
          const double *uNp1 = stk::mesh::field_data(velocityNp1, node );
          for ( int i = 0; i < nDim; ++i ) {
            const int offSet = i*current_num_face_nodes + ni;        
            p_c_face_velocity[offSet] = uNp1[i];
          }
        }

        // populate opposing face_node_ordinals
        opposingElementTopo.side_node_ordinals(opposingFaceOrdinal, ws_o_face_node_ordinals.begin());
        
        // gather opposing face data; sneak in second of connected nodes
        stk::mesh::Entity const* opposing_face_node_rels = bulk_data.begin_nodes(opposingFace);
        const int opposing_num_face_nodes = bulk_data.num_nodes(opposingFace);
        for ( int ni = 0; ni < opposing_num_face_nodes; ++ni ) {
          stk::mesh::Entity node = opposing_face_node_rels[ni];
          // set connected nodes
          connected_nodes[ni+current_num_face_nodes] = node;
          // gather; scalar
          p_o_diffFluxCoeff[ni] = *stk::mesh::field_data(*diffFluxCoeff_, node);
          // gather; vector
          const double *uNp1 = stk::mesh::field_data(velocityNp1, node );
          for ( int i = 0; i < nDim; ++i ) {
            const int offSet = i*opposing_num_face_nodes + ni;        
            p_o_face_velocity[offSet] = uNp1[i];
          }
        }
        
        // gather current element data
        stk::mesh::Entity const* current_elem_node_rels = bulk_data.begin_nodes(currentElement);
        const int current_num_elem_nodes = bulk_data.num_nodes(currentElement);
        for ( int ni = 0; ni < current_num_elem_nodes; ++ni ) {
          stk::mesh::Entity node = current_elem_node_rels[ni];
          // gather; vector
          const double *uNp1 = stk::mesh::field_data(velocityNp1, node );
          const double *coords = stk::mesh::field_data(*coordinates_, node);
          const int niNdim = ni*nDim;
          for ( int i = 0; i < nDim; ++i ) {
            p_c_elem_velocity[niNdim+i] = uNp1[i];
            p_c_elem_coordinates[niNdim+i] = coords[i];
          }
        }

        // gather opposing element data
        stk::mesh::Entity const* opposing_elem_node_rels = bulk_data.begin_nodes(opposingElement);
        const int opposing_num_elem_nodes = bulk_data.num_nodes(opposingElement);
        for ( int ni = 0; ni < opposing_num_elem_nodes; ++ni ) {
          stk::mesh::Entity node = opposing_elem_node_rels[ni];
          // gather; vector
          const double *uNp1 = stk::mesh::field_data(velocityNp1, node );
          const double *coords = stk::mesh::field_data(*coordinates_, node);
          const int niNdim = ni*nDim;
          for ( int i = 0; i < nDim; ++i ) {
            p_o_elem_velocity[niNdim+i] = uNp1[i];
            p_o_elem_coordinates[niNdim+i] = coords[i];
          }
        }

        // pointer to face data
        const double * c_areaVec = stk::mesh::field_data(*exposedAreaVec_, currentFace);
        const double * o_areaVec = stk::mesh::field_data(*exposedAreaVec_, opposingFace);
        const double * ncMassFlowRate = stk::mesh::field_data(*ncMassFlowRate_, currentFace);
        
        double c_amag = 0.0;
        double o_amag = 0.0;
        for ( int j = 0; j < nDim; ++j ) {
          const double c_axj = c_areaVec[currentGaussPointId*nDim+j];
          c_amag += c_axj*c_axj;
          // FIXME: choose first area vector on opposing surface? probably need something better for HO
          const double o_axj = o_areaVec[0*nDim+j];
          o_amag += o_axj*o_axj;
        }
        c_amag = std::sqrt(c_amag);
        o_amag = std::sqrt(o_amag);
        
        // now compute normal
        for ( int i = 0; i < nDim; ++i ) {
          p_cNx[i] = c_areaVec[currentGaussPointId*nDim+i]/c_amag;
          p_oNx[i] = o_areaVec[0*nDim+i]/o_amag;  
        }

        // project from side to element; method deals with the -1:1 isInElement range to the proper -0.5:0.5 CVFEM range
        meSCSCurrent->sidePcoords_to_elemPcoords(currentFaceOrdinal, 1, &currentIsoParCoords[0], &currentElementIsoParCoords[0]);
        meSCSOpposing->sidePcoords_to_elemPcoords(opposingFaceOrdinal, 1, &opposingIsoParCoords[0], &opposingElementIsoParCoords[0]);

        // compute dndx
        double scs_error = 0.0;
        meSCSCurrent->general_face_grad_op(currentFaceOrdinal, &currentElementIsoParCoords[0], 
                                           &p_c_elem_coordinates[0], &p_c_dndx[0], &ws_c_det_j[0], &scs_error);
        meSCSOpposing->general_face_grad_op(opposingFaceOrdinal, &opposingElementIsoParCoords[0], 
                                            &p_o_elem_coordinates[0], &p_o_dndx[0], &ws_o_det_j[0], &scs_error);

        // current inverse length scale; can loop over face nodes to avoid "nodesOnFace" array
        double currentInverseLength = 0.0;
        for ( int ic = 0; ic < current_num_face_nodes; ++ic ) {
          const int faceNodeNumber = ws_c_face_node_ordinals[ic];
          const int offSetDnDx = faceNodeNumber*nDim; // single intg. point
          for ( int j = 0; j < nDim; ++j ) {
            const double nxj = p_cNx[j];
            const double dndxj = p_c_dndx[offSetDnDx+j];
            currentInverseLength += dndxj*nxj;
          }
        }

        // opposing inverse length scale; can loop over face nodes to avoid "nodesOnFace" array
        double opposingInverseLength = 0.0;
        for ( int ic = 0; ic < opposing_num_face_nodes; ++ic ) {
          const int faceNodeNumber = ws_o_face_node_ordinals[ic];
          const int offSetDnDx = faceNodeNumber*nDim; // single intg. point
          for ( int j = 0; j < nDim; ++j ) {
            const double nxj = p_oNx[j];
            const double dndxj = p_o_dndx[offSetDnDx+j];
            opposingInverseLength += dndxj*nxj;
          }
        }

        // interpolate face data; current and opposing...
        meFCCurrent->interpolatePoint(
          sizeOfVectorField,
          &(dgInfo->currentIsoParCoords_[0]),
          &ws_c_face_velocity[0],
          &currentUBip[0]);
        
        meFCOpposing->interpolatePoint(
          sizeOfVectorField,
          &(dgInfo->opposingIsoParCoords_[0]),
          &ws_o_face_velocity[0],
          &opposingUBip[0]);

        double currentDiffFluxCoeffBip = 0.0;
        meFCCurrent->interpolatePoint(
          sizeOfScalarField,
          &(dgInfo->currentIsoParCoords_[0]),
          &ws_c_diffFluxCoeff[0],
          &currentDiffFluxCoeffBip);

        double opposingDiffFluxCoeffBip = 0.0;
        meFCOpposing->interpolatePoint(
          sizeOfScalarField,
          &(dgInfo->opposingIsoParCoords_[0]),
          &ws_o_diffFluxCoeff[0],
          &opposingDiffFluxCoeffBip);
        
        // compute viscous stress tensor; current
        double currentDiffFluxBip[3] = {0.0,0.0,0.0};
        for ( int ic = 0; ic < currentNodesPerElement; ++ic ) {

          const int offSetDnDx = ic*nDim; // single intg. point

          for ( int j = 0; j < nDim; ++j ) {

            const double nxj = p_cNx[j];
            const double dndxj = p_c_dndx[offSetDnDx+j];
            const double uxj = p_c_elem_velocity[ic*nDim+j];

            const double divUstress = 2.0/3.0*currentDiffFluxCoeffBip*dndxj*uxj*nxj*includeDivU_;

            for ( int i = 0; i < nDim; ++i ) {

              const double dndxi = p_c_dndx[offSetDnDx+i];
              const double uxi = p_c_elem_velocity[ic*nDim+i];

              // -mu*dui/dxj*Aj with divU
              currentDiffFluxBip[i] += -currentDiffFluxCoeffBip*dndxj*nxj*uxi + divUstress;

              // -mu*duj/dxi*Aj
              currentDiffFluxBip[i] += -currentDiffFluxCoeffBip*dndxi*nxj*uxj;
            }
          }
        }

        // compute viscous stress tensor; opposing
        double opposingDiffFluxBip[3] = {0.0,0.0,0.0};
        for ( int ic = 0; ic < opposingNodesPerElement; ++ic ) {

          const int offSetDnDx = ic*nDim; // single intg. point

          for ( int j = 0; j < nDim; ++j ) {

            const double nxj = p_oNx[j];
            const double dndxj = p_o_dndx[offSetDnDx+j];
            const double uxj = p_o_elem_velocity[ic*nDim+j];

            const double divUstress = 2.0/3.0*opposingDiffFluxCoeffBip*dndxj*uxj*nxj*includeDivU_;

            for ( int i = 0; i < nDim; ++i ) {

              const double dndxi = p_o_dndx[offSetDnDx+i];
              const double uxi = p_o_elem_velocity[ic*nDim+i];

              // -mu*dui/dxj*Aj with divU
              opposingDiffFluxBip[i] += -opposingDiffFluxCoeffBip*dndxj*nxj*uxi + divUstress;

              // -mu*duj/dxi*Aj
              opposingDiffFluxBip[i] += -opposingDiffFluxCoeffBip*dndxi*nxj*uxj;
            }
          }
        }

        // zero lhs/rhs
        for ( int p = 0; p < lhsSize; ++p )
          p_lhs[p] = 0.0;
        for ( int p = 0; p < rhsSize; ++p )
          p_rhs[p] = 0.0;

        // save mdot
        const double tmdot = ncMassFlowRate[currentGaussPointId];

        // compute penalty
        const double penaltyIp = 0.5*(currentDiffFluxCoeffBip*currentInverseLength + opposingDiffFluxCoeffBip*opposingInverseLength) 
          + std::abs(tmdot)/2.0;
   
        for ( int i = 0; i < nDim; ++i ) {
         
          // non conformal diffusive flux
          const double ncDiffFlux =  robinStyle_ ? -opposingDiffFluxBip[i] 
            : 0.5*(currentDiffFluxBip[i] - opposingDiffFluxBip[i]);

          // non conformal advection; find upwind (upwind prevails over Robin or DG approach)
          const double upwindUBip = tmdot > 0.0 ? currentUBip[i] : opposingUBip[i];
          const double ncAdv = upwindAdvection_ ? tmdot*upwindUBip : robinStyle_ ? tmdot*opposingUBip[i]
            : 0.5*tmdot*(currentUBip[i] + opposingUBip[i]);

          // assemble residual; form proper rhs index for current face assembly
          const int indexR = currentGaussPointId*nDim + i;
          p_rhs[indexR] -= ((dsFactor_*ncDiffFlux + penaltyIp*(currentUBip[i]-opposingUBip[i]))*c_amag + ncAdv);

          // set-up row for matrix
          const int rowR = indexR*(currentNodesPerFace+opposingNodesPerFace)*nDim;
        
          // sensitivities; current face; use general shape function for this single ip
          double lhsFac = penaltyIp*c_amag;
          meFCCurrent->general_shape_fcn(1, &currentIsoParCoords[0], &ws_c_general_shape_function[0]);
          for ( int ic = 0; ic < currentNodesPerFace; ++ic ) {
            const double r = p_c_general_shape_function[ic];
            const int nn = ic; // check this...
            p_lhs[rowR+nn*nDim+i] += r*lhsFac;
          }
        
          // sensitivities; opposing face; use general shape function for this single ip
          meFCOpposing->general_shape_fcn(1, &opposingIsoParCoords[0], &ws_o_general_shape_function[0]);
          for ( int ic = 0; ic < opposingNodesPerFace; ++ic ) {
            const double r = p_o_general_shape_function[ic];
            const int nn = ic + currentNodesPerFace;
            p_lhs[rowR+nn*nDim+i] -= r*lhsFac;
          }          
        }
        apply_coeff(connected_nodes, rhs, lhs, __FILE__);
      }
    }
  }
}

} // namespace nalu
} // namespace Sierra
