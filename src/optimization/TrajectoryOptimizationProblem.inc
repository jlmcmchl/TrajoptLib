// Copyright (c) TrajoptLib contributors

#pragma once

#include <cmath>
#include <exception>
#include <limits>
#include <variant>
#include <vector>

#include "DebugOptions.h"
#include "TrajectoryGenerationException.h"
#include "drivetrain/Drivetrain.h"
#include "obstacle/Obstacle.h"
#include "optimization/TrajectoryOptimizationProblem.h"
#include "path/HolonomicPath.h"
#include "path/Path.h"
#include "set/IntervalSet1d.h"
#include "set/Set2d.h"

namespace trajopt {

template <typename Opti>
TrajectoryOptimizationProblem<Opti>::TrajectoryOptimizationProblem(
    const Drivetrain& drivetrain, const Path& path)
    : drivetrain(drivetrain),
      path(path),
      waypointCount(path.Length()),
      trajectorySegmentCount(waypointCount - 1),
      controlIntervalTotal(path.ControlIntervalTotal()),
      sampleTotal(controlIntervalTotal + 1),
      opti(),
      dt(),
      x(),
      y(),
      theta(),
      dtSegments(),
      xSegments(),
      ySegments(),
      thetaSegments() {
  dt.reserve(controlIntervalTotal);
  x.reserve(sampleTotal);
  y.reserve(sampleTotal);
  theta.reserve(sampleTotal);

  for (size_t sampleIndex = 0; sampleIndex < sampleTotal; sampleIndex++) {
    x.push_back(opti.Variable());
    y.push_back(opti.Variable());
    theta.push_back(opti.Variable());
  }

  Expression totalT = 0;
  for (size_t trajectorySegmentIndex = 0;
       trajectorySegmentIndex < trajectorySegmentCount;
       trajectorySegmentIndex++) {
    Expression segmentDt = opti.Variable();
    size_t segmentControlIntervalCount =
        path.GetWaypoint(trajectorySegmentIndex + 1).controlIntervalCount;
    for (size_t segmentIntervalIndex = 0;
         segmentIntervalIndex < segmentControlIntervalCount;
         segmentIntervalIndex++) {
      dt.push_back(segmentDt);
    }
    totalT += segmentControlIntervalCount * segmentDt;
    opti.SubjectTo(segmentDt >= 0);
    opti.SetInitial(segmentDt, 5.0 / segmentControlIntervalCount);
  }
  opti.Minimize(totalT);

  dtSegments.reserve(trajectorySegmentCount);
  xSegments.reserve(waypointCount);
  ySegments.reserve(waypointCount);
  thetaSegments.reserve(waypointCount);
  xSegments.push_back({x[0]});
  ySegments.push_back({y[0]});
  thetaSegments.push_back({theta[0]});
  size_t sampleIndex = 1;
  for (size_t waypointIndex = 1; waypointIndex < waypointCount;
       waypointIndex++) {
    size_t controlIntervalCount =
        TrajectoryOptimizationProblem<Opti>::path.GetWaypoint(waypointIndex)
            .controlIntervalCount;
    std::vector<Expression> dtSegment;
    std::vector<Expression> xSegment;
    std::vector<Expression> ySegment;
    std::vector<Expression> thetaSegment;
    dtSegment.reserve(controlIntervalCount);
    xSegment.reserve(controlIntervalCount);
    ySegment.reserve(controlIntervalCount);
    thetaSegment.reserve(controlIntervalCount);
    for (size_t segmentSampleIndex = 0;
         segmentSampleIndex < controlIntervalCount; segmentSampleIndex++) {
      dtSegment.push_back(dt[(sampleIndex - 1) + segmentSampleIndex]);
      xSegment.push_back(x[sampleIndex + segmentSampleIndex]);
      ySegment.push_back(y[sampleIndex + segmentSampleIndex]);
      thetaSegment.push_back(theta[sampleIndex + segmentSampleIndex]);
    }
    dtSegments.push_back(dtSegment);
    xSegments.push_back(xSegment);
    ySegments.push_back(ySegment);
    thetaSegments.push_back(thetaSegment);
    sampleIndex += controlIntervalCount;
  }

  ApplyPathConstraints(opti, xSegments, ySegments, thetaSegments,
                       TrajectoryOptimizationProblem<Opti>::path);

  // ApplyObstacleConstraints(opti, xSegments, ySegments, thetaSegments,
  //         TrajectoryOptimizationProblem::drivetrain,
  //         TrajectoryOptimizationProblem::path);

  ApplyInitialGuessX(
      opti, x, y, theta,
      GenerateInitialGuessX(TrajectoryOptimizationProblem::path));
  // opti.SetInitial(X,
  // GenerateInitialGuessX(TrajectoryOptimizationProblem::path));
}

template <typename Opti>
void TrajectoryOptimizationProblem<Opti>::ApplyIntervalSet1dConstraint(
    Opti& opti, const Expression& scalar, const IntervalSet1d& set1d) {
  if (set1d.IsExact()) {
    opti.SubjectTo(scalar == set1d.lower);
  } else {
    if (set1d.IsLowerBounded()) {
      opti.SubjectTo(scalar >= set1d.lower);
    }
    if (set1d.IsUpperBounded()) {
      opti.SubjectTo(scalar <= set1d.upper);
    }
  }
}

template <typename Opti>
void TrajectoryOptimizationProblem<Opti>::ApplySet2dConstraint(
    Opti& opti, const Expression& vectorX, const Expression& vectorY,
    const Set2d& set2d) {
  if (set2d.IsRectangular()) {
    const RectangularSet2d& rectangularSet2d = set2d.GetRectangular();
    ApplyIntervalSet1dConstraint(opti, vectorX, rectangularSet2d.xBound);
    ApplyIntervalSet1dConstraint(opti, vectorY, rectangularSet2d.yBound);
  } else if (set2d.IsLinear()) {
    const LinearSet2d& linearSet2d = set2d.GetLinear();
    double sinTheta = std::sin(linearSet2d.theta);
    double cosTheta = std::cos(linearSet2d.theta);
    opti.SubjectTo(vectorX * sinTheta == vectorY * cosTheta);
  } else if (set2d.IsElliptical()) {
    const EllipticalSet2d& ellipticalSet2d = set2d.GetElliptical();
    const Expression scaledVectorXSquared =
        (vectorX * vectorX) /
        (ellipticalSet2d.xRadius * ellipticalSet2d.xRadius);
    const Expression scaledVectorYSquared =
        (vectorY * vectorY) /
        (ellipticalSet2d.yRadius * ellipticalSet2d.yRadius);
    const Expression lhs = scaledVectorXSquared + scaledVectorYSquared;
    switch (ellipticalSet2d.direction) {
      case EllipticalSet2d::Direction::kInside:
        opti.SubjectTo(lhs <= 1.0);
        break;
      case EllipticalSet2d::Direction::kCentered:
        opti.SubjectTo(lhs == 1.0);
        break;
      case EllipticalSet2d::Direction::kOutside:
        opti.SubjectTo(lhs >= 1.0);
        break;
    }
  } else if (set2d.IsCone()) {
    const ConeSet2d& coneSet2d = set2d.GetCone();
    opti.SubjectTo(vectorX * sin(coneSet2d.thetaBound.upper) >=  // NOLINT
                   vectorY * cos(coneSet2d.thetaBound.upper));   // NOLINT
    opti.SubjectTo(vectorX * sin(coneSet2d.thetaBound.lower) <=  // NOLINT
                   vectorY * cos(coneSet2d.thetaBound.lower));   // NOLINT
  }
}

template <typename Opti>
std::vector<double> TrajectoryOptimizationProblem<Opti>::SolutionValue(
    const Opti& opti, const std::vector<Expression>& rowVector) {
  std::vector<double> valueRowVector;
  valueRowVector.reserve(rowVector.size());
  for (auto& expression : rowVector) {
    valueRowVector.push_back(opti.SolutionValue(expression));
  }
  return valueRowVector;
}

template <typename Opti>
std::vector<std::vector<double>>
TrajectoryOptimizationProblem<Opti>::SolutionValue(
    const Opti& opti, const std::vector<std::vector<Expression>>& matrix) {
  std::vector<std::vector<double>> valueMatrix;
  valueMatrix.reserve(matrix.size());
  for (auto& row : matrix) {
    valueMatrix.push_back(
        TrajectoryOptimizationProblem<Opti>::SolutionValue(opti, row));
  }
  return valueMatrix;
}

template <typename Opti>
const typename TrajectoryOptimizationProblem<Opti>::BumperCornerPosition
TrajectoryOptimizationProblem<Opti>::SolveBumperCornerPosition(
    const Expression& x, const Expression& y, const Expression& theta,
    const ObstaclePoint& bumperCorner) {
  BumperCornerPosition position{0.0, 0.0};
  if (bumperCorner.x == 0.0 && bumperCorner.y == 0.0) {
    position.x = x;
    position.y = y;
  } else {
    double cornerDiagonal = std::hypot(bumperCorner.x, bumperCorner.y);
    double cornerAngle = std::atan2(bumperCorner.y, bumperCorner.x);
    position.x = x + cornerDiagonal * cos(cornerAngle + theta);  // NOLINT
    position.y = y + cornerDiagonal * sin(cornerAngle + theta);  // NOLINT
  }
  return position;
}

// https://www.desmos.com/calculator/cqmc1tjtsv
template <typename Expression, typename LineNumberType,
          typename PointNumberType>
Expression linePointDist(LineNumberType lineStartX, LineNumberType lineStartY,
                         LineNumberType lineEndX, LineNumberType lineEndY,
                         PointNumberType pointX, PointNumberType pointY) {
  Expression lX = lineEndX - lineStartX;
  Expression lY = lineEndY - lineStartY;
  Expression vX = pointX - lineStartX;
  Expression vY = pointY - lineStartY;
  Expression dot = vX * lX + vY * lY;
  Expression lNormSquared = lX * lX + lY * lY;
  Expression t = dot / lNormSquared;
  // Expression tBounded = std::fmax(std::fmin(t, 1), 0);
  Expression tBounded = t;
  Expression iX = (1 - tBounded) * lineStartX + tBounded * lineEndX;
  Expression iY = (1 - tBounded) * lineStartY + tBounded * lineEndY;
  Expression distSquared =
      (iX - pointX) * (iX - pointX) + (iY - pointY) * (iY - pointY);
  return distSquared;
}

template <typename Opti>
void TrajectoryOptimizationProblem<Opti>::ApplyObstacleConstraint(
    Opti& opti, const Expression& x, const Expression& y,
    const Expression& theta, const Obstacle& bumpers,
    const Obstacle& obstacle) {
  double distSquared = bumpers.safetyDistance + obstacle.safetyDistance;
  distSquared = distSquared * distSquared;
  size_t bumperCornerCount = bumpers.points.size();
  size_t obstacleCornerCount = obstacle.points.size();
  if (bumperCornerCount == 1 && obstacleCornerCount == 1) {
    // if the bumpers are only one point and the obstacle is also only one
    // point,
    const ObstaclePoint& bumperCorner = bumpers.points[0];
    const ObstaclePoint& obstaclePoint = obstacle.points[0];
    const BumperCornerPosition bumperCornerPosition =
        SolveBumperCornerPosition(x, y, theta, bumperCorner);
    Expression deltaX = obstaclePoint.x - bumperCornerPosition.x;
    Expression deltaY = obstaclePoint.y - bumperCornerPosition.y;
    Expression pointDistSquared = deltaX * deltaX + deltaY * deltaY;
    opti.SubjectTo(pointDistSquared >= distSquared);
  } else {
    for (size_t bumperCornerIndex = 0;
         bumperCornerIndex < bumperCornerCount - 1; bumperCornerIndex++) {
      const BumperCornerPosition startBumperCornerPosition =
          SolveBumperCornerPosition(x, y, theta,
                                    bumpers.points[bumperCornerIndex]);
      const BumperCornerPosition endBumperCornerPosition =
          SolveBumperCornerPosition(x, y, theta,
                                    bumpers.points[bumperCornerIndex + 1]);
      for (const ObstaclePoint& obstaclePoint : obstacle.points) {
        Expression dist = linePointDist<Expression>(
            startBumperCornerPosition.x, startBumperCornerPosition.y,
            endBumperCornerPosition.x, endBumperCornerPosition.y,
            obstaclePoint.x, obstaclePoint.y);
        opti.SubjectTo(dist >= distSquared);
      }
    }
    if (bumperCornerCount >=
        3) {  // must have at least three points to make a closed polygon
      const BumperCornerPosition startBumperCornerPosition =
          SolveBumperCornerPosition(x, y, theta,
                                    bumpers.points[bumperCornerCount - 1]);
      const BumperCornerPosition endBumperCornerPosition =
          SolveBumperCornerPosition(x, y, theta, bumpers.points[0]);
      for (const ObstaclePoint& obstaclePoint : obstacle.points) {
        Expression dist = linePointDist<Expression>(
            startBumperCornerPosition.x, startBumperCornerPosition.y,
            endBumperCornerPosition.x, endBumperCornerPosition.y,
            obstaclePoint.x, obstaclePoint.y);
        opti.SubjectTo(dist >= distSquared);
      }
    }

    for (size_t obstacleCornerIndex = 0;
         obstacleCornerIndex < obstacleCornerCount - 1; obstacleCornerIndex++) {
      double startObstacleCornerX = obstacle.points[obstacleCornerIndex].x;
      double startObstacleCornerY = obstacle.points[obstacleCornerIndex].y;
      double endObstacleCornerX = obstacle.points[obstacleCornerIndex + 1].x;
      double endObstacleCornerY = obstacle.points[obstacleCornerIndex + 1].y;
      for (const ObstaclePoint& bumperCorner : bumpers.points) {
        const BumperCornerPosition bumperCornerPosition =
            SolveBumperCornerPosition(x, y, theta, bumperCorner);
        Expression dist = linePointDist<Expression>(
            startObstacleCornerX, startObstacleCornerY, endObstacleCornerX,
            endObstacleCornerY, bumperCornerPosition.x, bumperCornerPosition.y);
        opti.SubjectTo(dist >=
                       bumpers.safetyDistance + obstacle.safetyDistance);
      }
    }
    if (obstacleCornerCount >= 3) {
      double startObstacleCornerX = obstacle.points[obstacleCornerCount - 1].x;
      double startObstacleCornerY = obstacle.points[obstacleCornerCount - 1].y;
      double endObstacleCornerX = obstacle.points[0].x;
      double endObstacleCornerY = obstacle.points[0].y;
      for (const ObstaclePoint& bumperCorner : bumpers.points) {
        const BumperCornerPosition bumperCornerPosition =
            SolveBumperCornerPosition(x, y, theta, bumperCorner);
        Expression dist = linePointDist<Expression>(
            startObstacleCornerX, startObstacleCornerY, endObstacleCornerX,
            endObstacleCornerY, bumperCornerPosition.x, bumperCornerPosition.y);
        opti.SubjectTo(dist >=
                       bumpers.safetyDistance + obstacle.safetyDistance);
      }
    }
  }
}

template <typename Opti>
void TrajectoryOptimizationProblem<Opti>::ApplyConstraint(
    Opti& opti, const Expression& x, const Expression& y,
    const Expression& theta, const Obstacle& bumpers,
    const Constraint& constraint) {
  if (constraint.IsTranslationConstraint()) {
    const TranslationConstraint& translationConstraint =
        constraint.GetTranslationConstraint();
    ApplySet2dConstraint(opti, x, y, translationConstraint.translationBound);
  } else if (constraint.IsHeadingConstraint()) {
    const HeadingConstraint& headingConstraint =
        constraint.GetHeadingConstraint();
    ApplyIntervalSet1dConstraint(opti, theta, headingConstraint.headingBound);
  } else if (constraint.IsPoseConstraint()) {
    const PoseConstraint& poseConstraint = constraint.GetPoseConstraint();
    auto translationConstraint =
        static_cast<TranslationConstraint>(poseConstraint);
    auto headingConstraint = static_cast<HeadingConstraint>(poseConstraint);
    ApplyConstraint(opti, x, y, theta, bumpers, translationConstraint);
    ApplyConstraint(opti, x, y, theta, bumpers, headingConstraint);
  } else /*if (constraint.IsObstacleConstraint())*/ {
    const ObstacleConstraint& obstacleConstraint =
        constraint.GetObstacleConstraint();
    ApplyObstacleConstraint(opti, x, y, theta, bumpers,
                            obstacleConstraint.obstacle);
  }
}

template <typename Opti>
void TrajectoryOptimizationProblem<Opti>::ApplyConstraints(
    Opti& opti, const typename Opti::Expression& x,
    const typename Opti::Expression& y, const typename Opti::Expression& theta,
    const Obstacle& bumpers, const std::vector<Constraint>& constraints) {
  for (const Constraint& constraint : constraints) {
    ApplyConstraint(opti, x, y, theta, bumpers, constraint);
  }
}

template <typename Opti>
void TrajectoryOptimizationProblem<Opti>::ApplyPathConstraints(
    Opti& opti, const std::vector<std::vector<Expression>>& xSegments,
    const std::vector<std::vector<Expression>>& ySegments,
    const std::vector<std::vector<Expression>>& thetaSegments,
    const Path& path) {
  ApplyConstraints(opti, xSegments[0][0], ySegments[0][0], thetaSegments[0][0],
                   path.bumpers, path.globalConstraints);
  ApplyConstraints(opti, xSegments[0][0], ySegments[0][0], thetaSegments[0][0],
                   path.bumpers, path.GetWaypoint(0).waypointConstraints);

  for (size_t waypointIndex = 1; waypointIndex < path.Length();
       waypointIndex++) {
    const Waypoint& waypoint = path.GetWaypoint(waypointIndex);
    size_t segmentSampleCount = waypoint.controlIntervalCount;
    size_t waypointSampleIndex = segmentSampleCount - 1;
    for (size_t segmentSampleIndex = 0;
         segmentSampleIndex < segmentSampleCount - 1; segmentSampleIndex++) {
      ApplyConstraints(opti, xSegments[waypointIndex][segmentSampleIndex],
                       ySegments[waypointIndex][segmentSampleIndex],
                       thetaSegments[waypointIndex][segmentSampleIndex],
                       path.bumpers, path.globalConstraints);
      ApplyConstraints(opti, xSegments[waypointIndex][segmentSampleIndex],
                       ySegments[waypointIndex][segmentSampleIndex],
                       thetaSegments[waypointIndex][segmentSampleIndex],
                       path.bumpers, waypoint.segmentConstraints);
    }
    ApplyConstraints(opti, xSegments[waypointIndex][waypointSampleIndex],
                     ySegments[waypointIndex][waypointSampleIndex],
                     thetaSegments[waypointIndex][waypointSampleIndex],
                     path.bumpers, path.globalConstraints);
    ApplyConstraints(opti, xSegments[waypointIndex][waypointSampleIndex],
                     ySegments[waypointIndex][waypointSampleIndex],
                     thetaSegments[waypointIndex][waypointSampleIndex],
                     path.bumpers, waypoint.waypointConstraints);
  }
}

inline void linspace(std::vector<double>& arry, size_t startIndex,
                     size_t endIndex, double startValue, double endValue) {
  size_t intervalCount = endIndex - startIndex;
  double delta = (endValue - startValue) / intervalCount;
  for (size_t index = 0; index < intervalCount; index++) {
    // arry[startIndex + index] = startValue + index * delta;
    arry.push_back(startValue + index * delta);
  }
}

template <typename Opti>
const typename TrajectoryOptimizationProblem<Opti>::InitialGuessX
TrajectoryOptimizationProblem<Opti>::GenerateInitialGuessX(const Path& path) {
  size_t waypointCount = path.Length();
  size_t controlIntervalTotal = path.ControlIntervalTotal();
  size_t sampleTotal = controlIntervalTotal + 1;
  InitialGuessX initialGuessX{{}, {}, {}};
  initialGuessX.x.reserve(sampleTotal);
  initialGuessX.y.reserve(sampleTotal);
  initialGuessX.theta.reserve(sampleTotal);
  initialGuessX.x.push_back(path.GetWaypoint(0).initialGuessPoints[0].x);
  initialGuessX.y.push_back(path.GetWaypoint(0).initialGuessPoints[0].y);
  initialGuessX.theta.push_back(
      path.GetWaypoint(0).initialGuessPoints[0].heading);
  size_t sampleIndex = 1;
  for (size_t waypointIndex = 1; waypointIndex < waypointCount;
       waypointIndex++) {
    const Waypoint& previousWaypoint = path.GetWaypoint(waypointIndex - 1);
    const Waypoint& waypoint = path.GetWaypoint(waypointIndex);
    const InitialGuessPoint& previousWaypointFinalInitialGuessPoint =
        previousWaypoint
            .initialGuessPoints[previousWaypoint.initialGuessPoints.size() - 1];
    size_t intervalCount = waypoint.controlIntervalCount;
    size_t guessPointCount = waypoint.initialGuessPoints.size();
    size_t guessSegmentIntervalCount = intervalCount / guessPointCount;
    linspace(initialGuessX.x, sampleIndex,
             sampleIndex + guessSegmentIntervalCount,
             previousWaypointFinalInitialGuessPoint.x,
             waypoint.initialGuessPoints[0].x);
    linspace(initialGuessX.y, sampleIndex,
             sampleIndex + guessSegmentIntervalCount,
             previousWaypointFinalInitialGuessPoint.y,
             waypoint.initialGuessPoints[0].y);
    linspace(initialGuessX.theta, sampleIndex,
             sampleIndex + guessSegmentIntervalCount,
             previousWaypointFinalInitialGuessPoint.heading,
             waypoint.initialGuessPoints[0].heading);
    for (size_t guessPointIndex = 1; guessPointIndex < guessPointCount - 1;
         guessPointIndex++) {  // if three or more guess points
      size_t previousGuessPointSampleIndex =
          sampleIndex + guessPointIndex * guessSegmentIntervalCount;
      size_t guessPointSampleIndex =
          sampleIndex + (guessPointIndex + 1) * guessSegmentIntervalCount;
      linspace(initialGuessX.x, previousGuessPointSampleIndex,
               guessPointSampleIndex,
               waypoint.initialGuessPoints[guessPointIndex - 1].x,
               waypoint.initialGuessPoints[guessPointIndex].x);
      linspace(initialGuessX.y, previousGuessPointSampleIndex,
               guessPointSampleIndex,
               waypoint.initialGuessPoints[guessPointIndex - 1].y,
               waypoint.initialGuessPoints[guessPointIndex].y);
      linspace(initialGuessX.theta, previousGuessPointSampleIndex,
               guessPointSampleIndex,
               waypoint.initialGuessPoints[guessPointIndex - 1].heading,
               waypoint.initialGuessPoints[guessPointIndex].heading);
    }
    if (guessPointCount > 1) {  // if two or more guess points
      size_t secondToLastGuessPointSampleIndex =
          sampleIndex + (guessPointCount - 1) * guessSegmentIntervalCount;
      size_t finalGuessPointSampleIndex = sampleIndex + intervalCount;
      linspace(initialGuessX.x, secondToLastGuessPointSampleIndex,
               finalGuessPointSampleIndex,
               waypoint.initialGuessPoints[guessPointCount - 2].x,
               waypoint.initialGuessPoints[guessPointCount - 1].x);
      linspace(initialGuessX.y, secondToLastGuessPointSampleIndex,
               finalGuessPointSampleIndex,
               waypoint.initialGuessPoints[guessPointCount - 2].y,
               waypoint.initialGuessPoints[guessPointCount - 1].y);
      linspace(initialGuessX.theta, secondToLastGuessPointSampleIndex,
               finalGuessPointSampleIndex,
               waypoint.initialGuessPoints[guessPointCount - 2].heading,
               waypoint.initialGuessPoints[guessPointCount - 1].heading);
    }
    sampleIndex += intervalCount;
  }
  return initialGuessX;
}

template <typename Opti>
void TrajectoryOptimizationProblem<Opti>::ApplyInitialGuessX(
    Opti& opti, std::vector<Expression>& x, std::vector<Expression>& y,
    std::vector<Expression>& theta, const InitialGuessX& initialGuessX) {
  size_t sampleTotal = x.size();
  for (size_t sampleIndex = 0; sampleIndex < sampleTotal; sampleIndex++) {
    opti.SetInitial(x[sampleIndex], initialGuessX.x[sampleIndex]);
    opti.SetInitial(y[sampleIndex], initialGuessX.y[sampleIndex]);
    opti.SetInitial(theta[sampleIndex], initialGuessX.theta[sampleIndex]);
  }
}

}  // namespace trajopt