#include "d2pgo.h"
#include "ARockPGO.hpp"
#include <d2common/solver/RelPoseFactor.hpp>
#include <d2common/solver/pose_local_parameterization.h>
#include <d2common/solver/angle_manifold.h>


namespace D2PGO {

void D2PGO::addFrame(const VINSFrame & frame_desc) {
    const Guard lock(state_lock);
    state.addFrame(frame_desc);
}

void D2PGO::addLoop(const Swarm::LoopEdge & loop_info) {
    const Guard lock(state_lock);
    loops.emplace_back(loop_info);
}

void D2PGO::solve() {
    const Guard lock(state_lock);
    SolverWrapper * solver;
    if (config.mode == PGO_MODE_NON_DIST) {
        solver = new CeresSolver(&state, config.ceres_options);
    } else if (config.mode == PGO_MODE_DISTRIBUTED_AROCK) {
        solver = new ARockPGO(&state, config.arock_config);
    }

    used_frames.clear();
    setupLoopFactors(solver);
    setupEgoMotionFactors(solver);

    if (config.mode == PGO_MODE_NON_DIST) {
        setStateProperties(solver->getProblem());
    }
}

void D2PGO::setupLoopFactors(SolverWrapper * solver) {
    auto loss_function = new ceres::HuberLoss(1.0);    
    for (auto loop : loops) {
        auto loop_factor = new RelPoseFactor(loop.relative_pose, loop.sqrt_information_matrix());
        auto res_info = RelPoseResInfo::create(loop_factor, 
            loss_function, loop.keyframe_id_a, loop.keyframe_id_b);
        used_frames.insert(loop.keyframe_id_a);
        used_frames.insert(loop.keyframe_id_b);
        solver->addResidual(res_info);
    }
}

void D2PGO::setupEgoMotionFactors(SolverWrapper * solver, int drone_id) {
    auto frames = state.getFrames(drone_id);
    auto traj = state.getTraj(drone_id);
    for (int i = 0; i < frames.size() - 1; i ++ ) {
        auto frame_a = frames[i];
        auto frame_b = frames[i + 1];
        // Swarm::TsType tsa = frame_a->stamp * 1e9;
        // Swarm::TsType tsb = frame_b->stamp * 1e9;
        // auto cov = traj.covariance_between_appro_ts(tsa, tsb);
        auto relpose6d = Swarm::Pose::DeltaPose(frame_a->odom.pose(), frame_b->odom.pose(), true);
        double len = relpose6d.pos().norm();
        Eigen::Matrix6d cov = Eigen::Matrix6d::Zero();
        cov.block<3, 3>(0, 0) = Matrix3d::Identity()*config.pos_covariance_per_meter*len 
            + 0.5*Matrix3d::Identity()*config.yaw_covariance_per_meter*len*len;
        cov.block<3, 3>(3, 3) = Matrix3d::Identity()*config.yaw_covariance_per_meter*len;
        Matrix6d sqrt_info = cov.inverse().cwiseAbs().cwiseSqrt();
        if (config.pgo_pose_dof == PGO_POSE_4D) {
            auto relpose4d = Swarm::Pose::DeltaPose(frame_a->odom.pose(), frame_b->odom.pose(), true);
            auto factor = RelPoseFactor4D::Create(relpose4d, sqrt_info.block<3,3>(0, 0), sqrt_info(5, 5));
            auto res_info = RelPoseResInfo::create(factor, nullptr, frame_a->frame_id, frame_b->frame_id);
            solver->addResidual(res_info);
        } else if (config.pgo_pose_dof == PGO_POSE_6D) {
            auto factor = new RelPoseFactor(relpose6d, sqrt_info);
            auto res_info = RelPoseResInfo::create(factor, nullptr, frame_a->frame_id, frame_b->frame_id);
            solver->addResidual(res_info);
        }

        used_frames.insert(frame_a->frame_id);
        used_frames.insert(frame_b->frame_id);
    }
}

void D2PGO::setupEgoMotionFactors(SolverWrapper * solver) {
    if (config.mode == PGO_MODE_NON_DIST) {
        for (auto drone_id : state.availableDrones()) {
            setupEgoMotionFactors(solver, drone_id);
        }
    } else if (config.mode >= PGO_MODE_DISTRIBUTED_AROCK) {
        setupEgoMotionFactors(solver, self_id);
    }
}

void D2PGO::setStateProperties(ceres::Problem & problem) {
    auto pose_local_param = new PoseLocalParameterization;
    auto pos_angle_manifold = PosAngleManifold::Create();
    for (auto frame_id : used_frames) {
        auto pointer = state.getPoseState(frame_id);
        if (!problem.HasParameterBlock(pointer)) {
            continue;
        }
        if (config.pgo_pose_dof == PGO_POSE_4D) {
            problem.SetManifold(pointer, pos_angle_manifold);
        } else {
            problem.SetParameterization(pointer, pose_local_param);
        }
    }
    if (config.mode == PGO_MODE_NON_DIST || 
            config.mode >= PGO_MODE_DISTRIBUTED_AROCK && self_id == main_id) {
        auto frame_id = state.headId(self_id);
        auto pointer = state.getPoseState(frame_id);
        problem.SetParameterBlockConstant(pointer);
    }
}

}