%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
% GTSAM Copyright 2010, Georgia Tech Research Corporation, 
% Atlanta, Georgia 30332-0415
% All Rights Reserved
% Authors: Frank Dellaert, et al. (see THANKS for the full author list)
% 
% See LICENSE for the license information
%
% @brief Example of a simple 2D localization example
% @author Frank Dellaert
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

%% Create the graph (defined in pose2SLAM.h, derived from NonlinearFactorGraph)
graph = pose2SLAM.Graph;

%% Add two odometry factors
import gtsam.*
odometry = Pose2(2.0, 0.0, 0.0); % create a measurement for both factors (the same in this case)
odometryNoise = noiseModel.Diagonal.Sigmas([0.2; 0.2; 0.1]); % 20cm std on x,y, 0.1 rad on theta
graph.addRelativePose(1, 2, odometry, odometryNoise);
graph.addRelativePose(2, 3, odometry, odometryNoise);

%% Add three "GPS" measurements
% We use Pose2 Priors here with high variance on theta
import gtsam.*
groundTruth = pose2SLAM.Values;
groundTruth.insertPose(1, Pose2(0.0, 0.0, 0.0));
groundTruth.insertPose(2, Pose2(2.0, 0.0, 0.0));
groundTruth.insertPose(3, Pose2(4.0, 0.0, 0.0));
model = noiseModel.Diagonal.Sigmas([0.1; 0.1; 10]);
for i=1:3
    graph.addPosePrior(i, groundTruth.pose(i), model);
end

%% Initialize to noisy points
initialEstimate = pose2SLAM.Values;
initialEstimate.insertPose(1, Pose2(0.5, 0.0, 0.2));
initialEstimate.insertPose(2, Pose2(2.3, 0.1,-0.2));
initialEstimate.insertPose(3, Pose2(4.1, 0.1, 0.1));

%% Optimize using Levenberg-Marquardt optimization with an ordering from colamd
result = graph.optimize(initialEstimate,0);

%% Plot Covariance Ellipses
marginals = graph.marginals(result);
P={};
for i=1:result.size()
    pose_i = result.pose(i);
    CHECK('pose_i.equals(groundTruth.pose(i)',pose_i.equals(groundTruth.pose(i),1e-4));
    P{i}=marginals.marginalCovariance(i);
end
