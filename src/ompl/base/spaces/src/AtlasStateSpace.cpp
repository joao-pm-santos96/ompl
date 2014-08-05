/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2014, Rice University
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Rice University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Caleb Voss */

#include "ompl/base/spaces/AtlasStateSpace.h"

#include "ompl/base/PlannerDataGraph.h"
#include "ompl/base/SpaceInformation.h"
#include "ompl/base/spaces/AtlasChart.h"
#include "ompl/util/Exception.h"

#include <boost/foreach.hpp>
#include <boost/math/special_functions/gamma.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/thread/lock_guard.hpp>

#include <eigen3/Eigen/Core>
#include <eigen3/Eigen/Geometry>

/// AtlasStateSampler

/// Public
ompl::base::AtlasStateSampler::AtlasStateSampler (const AtlasStateSpace &atlas)
: StateSampler(&atlas), atlas_(atlas)
{
}

void ompl::base::AtlasStateSampler::sampleUniform (State *state)
{
    Eigen::VectorXd r(atlas_.getManifoldDimension());
    AtlasChart *c;
    
    do
    {
        // Rejection sampling to find a point inside a chart's polytope
        do
        {
            // Pick a chart according to measure
            c = &atlas_.sampleChart();
            
            // Sample a point within rho_s of the center
            do
                r.setRandom();
            while (r.squaredNorm() > 1);
            r *= atlas_.getRho_s();
        }
        while (!c->inP(r));
        
        r = c->psi(r);
    }
    while (r.hasNaN() || atlas_.bigF(r).norm() > atlas_.getProjectionTolerance());
    
    // Extend polytope of neighboring chart wherever point is near the border
    c->borderCheck(c->psiInverse(r));
    state->as<AtlasStateSpace::StateType>()->setRealState(r, *c);
}

void ompl::base::AtlasStateSampler::sampleUniformNear (State *state, const State *near, const double distance)
{
    AtlasStateSpace::StateType *astate = state->as<AtlasStateSpace::StateType>();
    const AtlasStateSpace::StateType *anear = near->as<AtlasStateSpace::StateType>();
    Eigen::VectorXd n = anear->toVector();
    Eigen::VectorXd r;
    const AtlasChart *c = &anear->getChart();
    
    // Rejection sampling to find a point on the manifold
    do
    {
        // Sample within radius of distance
        Eigen::VectorXd uoffset(atlas_.getManifoldDimension());
        do
            uoffset.setRandom();
        while (uoffset.squaredNorm() > 1);
        const Eigen::VectorXd xoffset = c->phi(uoffset) - c->phi(Eigen::VectorXd::Zero(atlas_.getManifoldDimension()));
        r = c->psi(c->psiInverse(n + distance * xoffset.normalized()));
    }
    while (r.hasNaN() || atlas_.bigF(r).norm() > atlas_.getProjectionTolerance()  /*|| (r - n).squaredNorm() > distance*distance*/);
    astate->setRealState(r, *c);
    
    // It might belong to a different chart
    c = atlas_.owningChart(r);
    if (!c)
        c = &atlas_.newChart(r);
    else
        r = c->psi(c->psiInverse(r));
    astate->setRealState(r, *c);
}

void ompl::base::AtlasStateSampler::sampleGaussian (State *state, const State *mean, const double stdDev)
{
    AtlasStateSpace::StateType *astate = state->as<AtlasStateSpace::StateType>();
    const AtlasStateSpace::StateType *amean = mean->as<AtlasStateSpace::StateType>();
    Eigen::VectorXd r;
    const std::size_t k = atlas_.getManifoldDimension();
    
    // Rejection sampling to find a point in the ball
    const AtlasChart *c;
    boost::lock_guard<boost::mutex> lock(mutices_.rng_);
    do
    {
        c = &amean->getChart();
        const Eigen::VectorXd u = c->psiInverse(astate->toVector());
        Eigen::VectorXd rand(k);
        const double s = stdDev / std::sqrt(k);
        for (std::size_t i = 0; i < k; i++)
            rand[i] = rng_.gaussian(0, s);
        r = c->psi(u + rand);
    }
    while (r.hasNaN() || atlas_.bigF(r).norm() > atlas_.getProjectionTolerance());
    
    // It might belong to a different chart
    c = atlas_.owningChart(r);
    if (!c)
        c = &atlas_.newChart(r);
    else
        r = c->psi(c->psiInverse(r));
    astate->setRealState(r, *c);
}

/// AtlasValidStateSampler

/// Public

ompl::base::AtlasValidStateSampler::AtlasValidStateSampler (const AtlasStateSpacePtr &atlas, const SpaceInformation *si)
: ValidStateSampler(si), sampler_(*atlas)
{
}

bool ompl::base::AtlasValidStateSampler::sample (State *state)
{
    unsigned int fails = 0;
    do
        sampler_.sampleUniform(state);
    while (!si_->isValid(state) && ++fails < attempts_);
    
    return fails < attempts_;
}

bool ompl::base::AtlasValidStateSampler::sampleNear (State *state, const State *near, const double distance)
{
    unsigned int fails = 0;
    do
        sampler_.sampleUniformNear(state, near, distance);
    while (!si_->isValid(state) && ++fails < attempts_);
    
    return fails < attempts_;
}

/// AtlasMotionValidator

/// Public
ompl::base::AtlasMotionValidator::AtlasMotionValidator (SpaceInformation *si)
: MotionValidator(si), atlas_(*si->getStateSpace()->as<AtlasStateSpace>())
{
    checkSpace();
}

ompl::base::AtlasMotionValidator::AtlasMotionValidator (const SpaceInformationPtr &si)
: MotionValidator(si), atlas_(*si->getStateSpace()->as<AtlasStateSpace>())
{
    checkSpace();
}

bool ompl::base::AtlasMotionValidator::checkMotion (const State *s1, const State *s2) const
{
    // Simply invoke the manifold-traversing algorithm of the atlas
    return atlas_.followManifold(s1->as<AtlasStateSpace::StateType>(), s2->as<AtlasStateSpace::StateType>());
}

bool ompl::base::AtlasMotionValidator::checkMotion (const State *s1, const State *s2, std::pair<State *, double> &lastValid) const
{
    // Invoke the advanced version of the manifold-traversing algorithm to save intermediate states
    std::vector<AtlasStateSpace::StateType *> stateList;
    const bool noCollisionChecking = true;
    bool reached = atlas_.followManifold(s1->as<AtlasStateSpace::StateType>(), s2->as<AtlasStateSpace::StateType>(),
        noCollisionChecking, &stateList);
    
    // Go back and collision check by hand
    const StateValidityCheckerPtr &svc = si_->getStateValidityChecker();
    double length = 0;
    bool foundCollision = false;
    for (std::size_t i = 1; i < stateList.size(); i++)
    {
        if (!foundCollision && !svc->isValid(stateList[i]))
        {
            // This is the first point in collision; save the previous state and length so far
            foundCollision = true;
            lastValid.second = length;
            if (lastValid.first)
                atlas_.copyState(lastValid.first, stateList[i-1]);
        }
        length += atlas_.distance(stateList[i-1], stateList[i]);
        atlas_.freeState(stateList[i-1]);
    }
    atlas_.freeState(stateList.back());
    
    // Compute the interpolation parameter of the last valid state
    if (!reached)
        lastValid.second /= length;
    
    return reached;
}

/// Private
void ompl::base::AtlasMotionValidator::checkSpace (void)
{
    if (!dynamic_cast<AtlasStateSpace *>(si_->getStateSpace().get()))
        throw ompl::Exception("AtlasMotionValidator's SpaceInformation needs to use an AtlasStateSpace!");
}

/// AtlasStateSpace::StateType

/// Public
ompl::base::AtlasStateSpace::StateType::StateType (const unsigned int dimension)
: RealVectorStateSpace::StateType(), chart_(NULL), dimension_(dimension)
{
    // Mimic what RealVectorStateSpace::allocState() would have done
    values = new double[dimension_];
}

ompl::base::AtlasStateSpace::StateType::~StateType(void)
{
    // Mimic what RealVectorStateSpace::freeState() would have done
    delete [] values;
}

void ompl::base::AtlasStateSpace::StateType::setRealState (const Eigen::VectorXd &x, const AtlasChart &c)
{
    {
        boost::lock_guard<boost::mutex> lock(mutices_.vector_);
        for (std::size_t i = 0; i < dimension_; i++)
            (*this)[i]  = x[i];
    }
    boost::lock_guard<boost::mutex> lock(mutices_.chart_);
    if (chart_ != &c)
    {
        if (chart_)
            chart_->disown(this);
        c.own(this);
    }
    chart_ = &c;
}

Eigen::VectorXd ompl::base::AtlasStateSpace::StateType::toVector (void) const
{
    boost::lock_guard<boost::mutex> lock(mutices_.vector_);
    Eigen::VectorXd x(dimension_);
    for (std::size_t i = 0; i < dimension_; i++)
        x[i] = (*this)[i];
    return x;
}

const ompl::base::AtlasChart &ompl::base::AtlasStateSpace::StateType::getChart (void) const
{
    return *chart_;
}

const ompl::base::AtlasChart *ompl::base::AtlasStateSpace::StateType::getChart_safe (void) const
{
    return chart_;
}

void ompl::base::AtlasStateSpace::StateType::setChart (const AtlasChart &c, const bool fast)
{
    boost::lock_guard<boost::mutex> lock(mutices_.chart_);
    if (chart_ != &c)
    {
        if (chart_ && !fast)
            chart_->disown(this);
        c.own(this);
    }
    chart_ = &c;
}

/// AtlasStateSpace

/// Public
ompl::base::AtlasStateSpace::AtlasStateSpace (const unsigned int dimension, const ConstraintsFn constraints, const JacobianFn jacobian)
: RealVectorStateSpace(dimension),
    bigF(constraints),
    bigJ(jacobian ? jacobian : boost::bind(&AtlasStateSpace::numericalJacobian, this, boost::lambda::_1)),
    n_(dimension), delta_(0.02), epsilon_(0.1), exploration_(0.5), lambda_(2),
    projectionTolerance_(1e-8), projectionMaxIterations_(300), maxChartsPerExtension_(200), monteCarloSampleCount_(100), setup_(false), noAtlas_(true)
{
    Eigen::initParallel();
    setName("Atlas" + RealVectorStateSpace::getName());
    
    // Infer the manifold dimension
    Eigen::VectorXd zero = Eigen::VectorXd::Zero(n_);
    k_ = n_ - bigF(zero).size();
    if (k_ <= 0)
        throw ompl::Exception("Too many constraints! The manifold must be at least 1-dimensional.");
    if (!jacobian)
        OMPL_INFORM("Atlas: Jacobian not given. Using numerical methods to compute it. (May be slower and/or less accurate.)");
    else if (bigJ(zero).rows() != bigF(zero).size() || bigJ(zero).cols() != n_)
        throw ompl::Exception("Dimensions of the Jacobian are incorrect! Should be n-k by n, where n, k are the ambient, manifold dimensions.");
    
    
    setRho(0.1);
    setAlpha(M_PI/16);
    
    ballMeasure_ = std::pow(std::sqrt(M_PI), k_) / boost::math::tgamma(k_/2.0 + 1);
    
    // Generate random samples within the ball
    samples_.resize(monteCarloSampleCount_);
    for (std::size_t i = 0; i < samples_.size(); i++)
    {
        do
            samples_[i] = Eigen::VectorXd::Random(k_);
        while (samples_[i].squaredNorm() > 1);
    }
}

ompl::base::AtlasStateSpace::~AtlasStateSpace (void)
{
    for (std::size_t i = 0; i < charts_.size(); i++)
        delete charts_[i];
}

void ompl::base::AtlasStateSpace::stopBeingAnAtlas (const bool yes)
{
    noAtlas_ = yes;
}

void ompl::base::AtlasStateSpace::setup (void)
{
    if (setup_)
        return;
    
    if (!si_)
        throw ompl::Exception("Must associate a SpaceInformation object to the AtlasStateSpace via setStateInformation() before use.");
    RealVectorStateSpace::setup();
    setup_ = true;
}

void ompl::base::AtlasStateSpace::clear (void)
{
    // Copy the list of charts
    std::vector<AtlasChart *> oldCharts;
    {
        boost::lock_guard<boost::mutex> lock(mutices_.chartsVector_);
        for (std::size_t i = 0; i < charts_.size(); i++)
        {
            oldCharts.push_back(charts_[i]);
        }
        
        charts_.clear();
    }
    
    for (std::size_t i = 0; i < oldCharts.size(); i++)
    {
        if (oldCharts[i]->isAnchor())
        {
            // Reincarnate the chart
            oldCharts[i]->substituteChart(anchorChart(oldCharts[i]->phi(Eigen::VectorXd::Zero(k_))));
        }
        delete oldCharts[i];
    }
}

void ompl::base::AtlasStateSpace::setSpaceInformation (const SpaceInformationPtr &si)
{
    // Check that the object is valid
    if (!si)
        throw ompl::Exception("SpaceInformationPtr associated to the AtlasStateSpace was NULL.");
    if (si->getStateSpace().get() != this)
        throw ompl::Exception("SpaceInformation for AtlasStateSpace must be constructed from the same space object.");
    
    // Save only a raw pointer to prevent a cycle
    si_ = si.get();
    
    si_->setStateValidityCheckingResolution(delta_);
}

void ompl::base::AtlasStateSpace::setDelta (const double delta)
{
    if (delta <= 0)
        throw ompl::Exception("Please specify a positive delta.");
    delta_  = delta;
    
    if (si_)
        si_->setStateValidityCheckingResolution(delta_);
}

void ompl::base::AtlasStateSpace::setEpsilon (const double epsilon)
{
    if (epsilon <= 0)
        throw ompl::Exception("Please specify a positive epsilon.");
    epsilon_ = epsilon;
}

void ompl::base::AtlasStateSpace::setRho (const double rho)
{
    if (rho <= 0)
        throw ompl::Exception("Please specify a positive rho.");
    rho_ = rho;
    rho_s_ = rho_ / std::pow(1 - exploration_, 1.0/k_);
}

void ompl::base::AtlasStateSpace::setAlpha (const double alpha)
{
    if (alpha <= 0 || alpha >= M_PI_2)
        throw ompl::Exception("Please specify an alpha within the range (0,pi/2).");
    cos_alpha_ = std::cos(alpha);
}

void ompl::base::AtlasStateSpace::setExploration (const double exploration)
{
    if (exploration < 0 || exploration >= 1)
        throw ompl::Exception("Please specify an exploration value within the range [0,1).");
    exploration_ = exploration;
    
    // Update sampling radius
    setRho(rho_);
}

void ompl::base::AtlasStateSpace::setLambda (const double lambda)
{
    if (lambda <= 1)
        throw ompl::Exception("Please specify a lambda greater than 1.");
    lambda_ = lambda;
}

void ompl::base::AtlasStateSpace::setProjectionTolerance (const double tolerance)
{
    if (tolerance <= 0)
        throw ompl::Exception("Please specify a projection tolerance greater than 0.");
    projectionTolerance_ = tolerance;
}

void ompl::base::AtlasStateSpace::setProjectionMaxIterations (const unsigned int iterations)
{
    if (iterations == 0)
        throw ompl::Exception("Please specify a positive maximum projection iteration count.");
    projectionMaxIterations_ = iterations;
}

void ompl::base::AtlasStateSpace::setMaxChartsPerExtension (const unsigned int charts)
{
    maxChartsPerExtension_ = charts;
}

void ompl::base::AtlasStateSpace::setMonteCarloSampleCount (const unsigned int count)
{
    monteCarloSampleCount_ = count;;
}

double ompl::base::AtlasStateSpace::getDelta (void) const
{
    return delta_;
}

double ompl::base::AtlasStateSpace::getEpsilon (void) const
{
    return epsilon_;
}

double ompl::base::AtlasStateSpace::getRho (void) const
{
    return rho_;
}

double ompl::base::AtlasStateSpace::getAlpha (void) const
{
    return std::acos(cos_alpha_);
}

double ompl::base::AtlasStateSpace::getExploration (void) const
{
    return exploration_;
}

double ompl::base::AtlasStateSpace::getLambda (void) const
{
    return lambda_;
}

double ompl::base::AtlasStateSpace::getRho_s (void) const
{
    return rho_s_;
}

double ompl::base::AtlasStateSpace::getProjectionTolerance (void) const
{
    return projectionTolerance_;
}

unsigned int ompl::base::AtlasStateSpace::getProjectionMaxIterations (void) const
{
    return projectionMaxIterations_;
}

unsigned int ompl::base::AtlasStateSpace::getMaxChartsPerExtension (void) const
{
    return maxChartsPerExtension_;
}

unsigned int ompl::base::AtlasStateSpace::getMonteCarloSampleCount (void) const
{
    return monteCarloSampleCount_;
}

unsigned int ompl::base::AtlasStateSpace::getAmbientDimension (void) const
{
    return n_;
}

unsigned int ompl::base::AtlasStateSpace::getManifoldDimension (void) const
{
    return k_;
}

ompl::base::AtlasChart &ompl::base::AtlasStateSpace::anchorChart (const Eigen::VectorXd &xorigin) const
{
    return newChart(xorigin, true);
}

ompl::base::AtlasChart &ompl::base::AtlasStateSpace::sampleChart (void) const
{
    double r;
    {
        boost::lock_guard<boost::mutex> lock(mutices_.rng_);
        r = rng_.uniform01();
    }
    
    boost::lock_guard<boost::mutex> lock1(mutices_.chartsVector_);
    boost::lock_guard<boost::mutex> lock2(mutices_.chartsWeights_);
    if (charts_.size() < 1)
        throw ompl::Exception("Atlas sampled before any charts were made. Use AtlasStateSpace::newChart() first.");
    return *charts_.sample(r);
}

ompl::base::AtlasChart *ompl::base::AtlasStateSpace::owningChart (const Eigen::VectorXd &x, const AtlasChart *const neighbor) const
{
    // Use hint first if available
    AtlasChart *bestC = NULL;
    if (neighbor)
    {
        bestC = const_cast<AtlasChart *>(neighbor->owningNeighbor(x));
        if (bestC)
            return bestC;
    }
    
    // If not found, search through all charts for the best match
    double best = delta_;
    for (std::size_t i = 0; i < charts_.size(); i++)
    {
        // The point must lie in the chart's validity region and polytope
        AtlasChart &c = *charts_[i];
        const Eigen::VectorXd psiInvX = c.psiInverse(x);
        const Eigen::VectorXd psiPsiInvX = c.psi(psiInvX);
        if ((c.phi(psiInvX) - psiPsiInvX).norm() < epsilon_ && psiInvX.norm() < rho_ && c.inP(psiInvX))
        {
            // The closer the point to where the chart puts it, the better
            double err = (psiPsiInvX - x).norm();
            if (err < best)
            {
                bestC = &c;
                best = err;
            }
        }
    }
    
    return bestC;
}

ompl::base::AtlasChart &ompl::base::AtlasStateSpace::newChart (const Eigen::VectorXd &xorigin, const bool anchor) const
{
    AtlasChart &addedC = *new AtlasChart(*this, xorigin, anchor);
    std::vector<AtlasChart *> oldCharts;
    {
        boost::lock_guard<boost::mutex> lock(mutices_.chartsVector_);
        for (std::size_t i = 0; i < charts_.size(); i++)
        {
            oldCharts.push_back(charts_[i]);
        }
        addedC.setID(charts_.size());
        charts_.add(&addedC, addedC.getMeasure());
    }
    
    // Ensure all charts respect boundaries of the new one, and vice versa
    for (std::size_t i = 0; i < oldCharts.size(); i++)
    {
        // If the two charts are near enough, introduce a boundary
        AtlasChart &c = *oldCharts[i];
        if ((c.phi(Eigen::VectorXd::Zero(k_)) - addedC.phi(Eigen::VectorXd::Zero(k_))).norm() < 2*rho_)
            AtlasChart::generateHalfspace(c, addedC);
    }
    
    return addedC;
}

Eigen::VectorXd ompl::base::AtlasStateSpace::dichotomicSearch (const AtlasChart &c, const Eigen::VectorXd &xinside, Eigen::VectorXd xoutside) const
{
    // Cut the distance in half, moving toward xinside until we are inside the chart
    while (!c.inP(c.psiInverse(xoutside)))
        xoutside = 0.5 * (xinside + xoutside);
    
    return xoutside;
}

void ompl::base::AtlasStateSpace::updateMeasure (const AtlasChart &c) const
{
    boost::lock_guard<boost::mutex> lock(mutices_.chartsWeights_);
    charts_.update(charts_.getElements()[c.getID()], c.getMeasure());
}

double ompl::base::AtlasStateSpace::getMeasureKBall (void) const
{
    return ballMeasure_;
}

const std::vector<Eigen::VectorXd> &ompl::base::AtlasStateSpace::getMonteCarloSamples (void) const
{
    return samples_;
}

std::size_t ompl::base::AtlasStateSpace::getChartCount (void) const
{
    return charts_.size();
}

/** \brief Traverse the manifold from \a from toward \a to. Returns true if we reached \a to, and false if
    * we stopped early for any reason, such as a collision or traveling too far. No collision checking is performed
    * if \a interpolate is true. If \a stateList is not NULL, the sequence of intermediates is saved to it, including
    * a copy of \a from, as well as the final state. */
bool ompl::base::AtlasStateSpace::followManifold (const StateType *from, const StateType *to, const bool interpolate,
                                                  std::vector<StateType *> *const stateList) const
{
    unsigned int chartsCreated = 0;
    AtlasChart *c = const_cast<AtlasChart *>(&from->getChart());
    const StateValidityCheckerPtr &svc = si_->getStateValidityChecker();
    StateType *temp = allocState()->as<StateType>();
    
    // Save a copy of the from state
    if (stateList)
    {
        stateList->clear();
        StateType *fromCopy = allocState()->as<StateType>();
        copyState(fromCopy, from);
        stateList->push_back(fromCopy);
    }
    
    Eigen::VectorXd x_n, x_r, x_j, x_0, u_n, u_r, u_j;
    std::list<Eigen::VectorXd> lastTenX;
    double lastTenD = 0;
    x_n = from->toVector();
    x_r = to->toVector();
    
    // We will stop if we exit the ball of radius d_0 centered at x_0
    x_0 = x_j = x_n;
    double d_0 = (x_n - x_r).norm();
    double d = 0;
    
    // Project from and to points onto the chart
    u_n = c->psiInverse(x_n);
    u_r = c->psiInverse(x_r);
    
    // Deviation: we can't know whether we're in 'explore' mode. We typically actually want to reach the specified sample, not just to grow the atlas.
    /*
    if (explore)
    {
        u_r = u_n + d_0*(u_r - u_n).normalized();  // Note the difference between this and the pseudocode (line 8): it's a subtle mistake
        x_r = c.phi(u_r);
    }
    */
    
    //bool chartCreated = false;    // Unused for now
    while ((u_r - u_n).squaredNorm() > delta_*delta_)
    {
        lastTenX.push_back(x_n);
        lastTenD += (x_n - x_j).norm();
        if (lastTenX.size() > 10)
        {
            lastTenD -= (lastTenX.front() - *boost::next(lastTenX.begin())).norm();
            lastTenX.pop_front();
            if ((lastTenX.front() - lastTenX.back()).norm() < 0.1*lastTenD)
            {
                // No way to get out
                OMPL_DEBUG("Probably got stuck in local minimum.");
                break;
            }
        }
        
        
        // Step by delta toward the target and project
        u_j = u_n + delta_*(u_r - u_n).normalized();    // Note the difference to pseudocode (line 13): a similar mistake to line 8
        x_j = c->psi(u_j);
        
        double d_s = (x_n - x_j).norm();
        bool changedChart = false;
        
        // Collision check unless interpolating
        temp->setRealState(x_j, *c);
        if (!interpolate && !svc->isValid(temp))
            break;
        
        if (((x_j - c->phi(u_j)).squaredNorm() > epsilon_*epsilon_ || delta_/d_s < cos_alpha_ || u_j.squaredNorm() > rho_*rho_))
        {
            // Left the validity region of the chart; make a new one
            if (u_n.norm() < 1e-6)
            {
               // Point we want to center the new chart on is already a chart center
                c = &newChart(dichotomicSearch(*c, x_n, x_j));  // See paper's discussion of probabilistic completeness; this was left out of pseudocode
            }
            else
            {
                c = &newChart(x_n);
            }
            chartsCreated++;
            changedChart = true;
            //chartCreated = true;  // Again, unused
        }
        else if (!c->inP(u_j))
        {
            // Left the polytope of the chart; find the correct chart
            AtlasChart *newc = owningChart(c->phi(u_j), c);   // Paper says this is a neighboring chart. That may not always be true, esp. for large delta
            
            // Deviation: If rho is too big, charts have gaps between them; this fixes it on the fly
            if (!newc)
            {
                OMPL_DEBUG("Fell between the cracks! Patching in a new chart now.");
                c->shrinkRadius();
                updateMeasure(*c);
                c = &newChart(x_n);
                chartsCreated++;
            }
            else
            {
                c = newc;
            }
            
            // Deviation: again we can't know about 'explore' mode or whether a chart is in the current tree
            /*
            if (!interpolate && (chartCreated || (!explore && inTree(c))))
                break;
            */
            
            changedChart = true;
        }
        
        if (changedChart)
        {
            // Re-project onto the different chart
            u_j = c->psiInverse(x_j);
            u_r = c->psiInverse(x_r);
            
            // Deviation: 'explore' mode issue, once again
            /*
            if (explore)
            {
                u_n = c->psiInverse(x_n);
                u_r = u_n + (x_r - x_n).norm() * (u_r - u_n).normalized();  // Note the difference to pseudocode (line 37). More severe issue than line 8.
                x_r = c->phi(u_r);
            }
            */
        }
        
        // Deviation: No control over the planner's tree, so we'll just keep the state in a list, if requested
        if (stateList)
        {
            StateType *intermediate = allocState()->as<StateType>();
            intermediate->setRealState(x_j, *c);
            stateList->push_back(intermediate);
        }
        
        // Update iteration variables
        u_n = u_j;
        x_n = x_j;
        
        // Check stopping criteria regarding how far we've gone
        d += d_s;
        if ((x_0 - x_j).norm() > d_0 || d > lambda_*d_0 || chartsCreated > maxChartsPerExtension_)
            break;
    }
    if (chartsCreated > maxChartsPerExtension_)
        OMPL_DEBUG("Stopping extension early b/c too many charts created.");
    const bool reached = ((x_r - x_n).squaredNorm() < delta_*delta_);
    
    // Append a copy of the target state, since we're within delta, but didn't hit it exactly
    if (reached && stateList)
    {
        StateType *toCopy = allocState()->as<StateType>();
        copyState(toCopy, to);
        stateList->push_back(toCopy);
    }
    
    freeState(temp);
    return reached;
}

void ompl::base::AtlasStateSpace::dumpMesh (std::ostream &out) const
{
    std::stringstream v, f;
    std::size_t vcount = 0;
    std::size_t fcount = 0;
    std::vector<Eigen::VectorXd> vertices;
    for (std::size_t i = 0; i < charts_.size(); i++)
    {
        // Write the vertices and the faces
        std::cout << "\rDumping chart " << i << std::flush;
        const AtlasChart &c = *charts_[i];
        c.toPolygon(vertices);
        std::stringstream poly;
        std::size_t fvcount = 0;
        for (std::size_t j = 0; j < vertices.size(); j++)
        {
            v << vertices[j].transpose() << "\n";
            poly << vcount++ << " ";
            fvcount++;
        }
        
        if (fvcount > 2)
        {
            f << fvcount << " " << poly.str() << "\n";
            fcount += 1;
        }
    }
    std::cout << "\n";
    out << "ply\n";
    out << "format ascii 1.0\n";
    out << "element vertex " << vcount << "\n";
    out << "property float x\n";
    out << "property float y\n";
    out << "property float z\n";
    out << "element face " << fcount << "\n";
    out << "property list uint uint vertex_index\n";
    out << "end_header\n";
    out << v.str() << f.str();
}

void ompl::base::AtlasStateSpace::dumpGraph (const PlannerData::Graph &graph, std::ostream &out, const bool asIs) const
{
    std::stringstream v, f;
    std::size_t vcount = 0;
    std::size_t fcount = 0;
    
    BOOST_FOREACH (PlannerData::Graph::Edge edge, boost::edges(graph))
    {
        std::vector<StateType *> stateList;
        const State *source = boost::get(vertex_type, graph, boost::source(edge, graph))->getState();
        const State *target = boost::get(vertex_type, graph, boost::target(edge, graph))->getState();
        
        if (!asIs)
            followManifold(source->as<StateType>(), target->as<StateType>(), true, &stateList);
        if (asIs || stateList.size() == 1)
        {
            v << source->as<StateType>()->toVector().transpose() << "\n";
            v << target->as<StateType>()->toVector().transpose() << "\n";
            v << source->as<StateType>()->toVector().transpose() << "\n";
            vcount += 3;
            f << 3 << " " << vcount-3 << " " << vcount-2 << " " << vcount-1 << "\n";
            fcount++;
            continue;
        }
        StateType *from = stateList[0];
        v << from->toVector().transpose() << "\n";
        vcount++;
        bool reset = true;
        for (std::size_t i = 1; i < stateList.size(); i++)
        {
            StateType *to = stateList[i];
            StateType *from = stateList[i-1];
            v << to->toVector().transpose() << "\n";
            v << from->toVector().transpose() << "\n";
            vcount += 2;
            f << 3 << " " << (reset ? vcount-3 : vcount-4) << " " << vcount-2 << " " << vcount-1 << "\n";
            fcount++;
            reset = false;
        }
    }
    
    out << "ply\n";
    out << "format ascii 1.0\n";
    out << "element vertex " << vcount << "\n";
    out << "property float x\n";
    out << "property float y\n";
    out << "property float z\n";
    out << "element face " << fcount << "\n";
    out << "property list uint uint vertex_index\n";
    out << "end_header\n";
    out << v.str() << f.str();
}

void ompl::base::AtlasStateSpace::dumpPath (ompl::geometric::PathGeometric &path, std::ostream &out, const bool asIs) const
{
    std::stringstream v, f;
    std::size_t vcount = 0;
    std::size_t fcount = 0;
    
    const std::vector<State *> &waypoints = path.getStates();
    for (std::size_t i = 0; i < waypoints.size()-1; i++)
    {
        std::vector<StateType *> stateList;
        State *source = waypoints[i];
        State *target = waypoints[i+1];
        
        if (!asIs)
            followManifold(source->as<StateType>(), target->as<StateType>(), true, &stateList);
        if (asIs || stateList.size() == 1)
        {
            v << source->as<StateType>()->toVector().transpose() << "\n";
            v << target->as<StateType>()->toVector().transpose() << "\n";
            v << source->as<StateType>()->toVector().transpose() << "\n";
            vcount += 3;
            f << 3 << " " << vcount-3 << " " << vcount-2 << " " << vcount-1 << "\n";
            fcount++;
            continue;
        }
        StateType *from = stateList[0];
        v << from->toVector().transpose() << "\n";
        vcount++;
        bool reset = true;
        for (std::size_t i = 1; i < stateList.size(); i++)
        {
            StateType *to = stateList[i];
            StateType *from = stateList[i-1];
            v << to->toVector().transpose() << "\n";
            v << from->toVector().transpose() << "\n";
            vcount += 2;
            f << 3 << " " << (reset ? vcount-3 : vcount-4) << " " << vcount-2 << " " << vcount-1 << "\n";
            fcount++;
            reset = false;
        }
    }
    
    out << "ply\n";
    out << "format ascii 1.0\n";
    out << "element vertex " << vcount << "\n";
    out << "property float x\n";
    out << "property float y\n";
    out << "property float z\n";
    out << "element face " << fcount << "\n";
    out << "property list uint uint vertex_index\n";
    out << "end_header\n";
    out << v.str() << f.str();
}

void ompl::base::AtlasStateSpace::interpolate (const State *from, const State *to, const double t, State *state) const
{
    if (noAtlas_)
    {
        RealVectorStateSpace::interpolate(from, to, t, state);
        return;
    }
    
    // Traverse the manifold and save all the intermediate states
    std::vector<StateType *> stateList;
    const bool noCollisionChecking = true;
    if (!followManifold(from->as<StateType>(), to->as<StateType>(), noCollisionChecking, &stateList))
    {
        // If we cannot reach the to state, we cannot know how far away it is. Assume infinite distance
        // and just return the to state for all t > 0.
        copyState(state, t > 0 ? to : from);
        return;
    }
    
    // Compute the state at time t
    fastInterpolate(stateList, t, state);
    
    // We are resposible for freeing these states
    for (std::size_t i = 0; i < stateList.size(); i++)
        freeState(stateList[i]);
}

void ompl::base::AtlasStateSpace::fastInterpolate (const std::vector<StateType *> &stateList, const double t, State *state) const
{
    std::size_t n = stateList.size();
    double *d = new double[n];

    // Compute partial sums of distances between intermediate states
    d[0] = 0;
    for (std::size_t i = 1; i < n; i++)
        d[i] = d[i-1] + distance(stateList[i-1], stateList[i]);
    
    // Find the two adjacent states between which lies t
    std::size_t i = 0;
    double tt;
    if (d[n-1] == 0)
    {
        // Corner case where total distance is near 0; prevents division by 0
        i = n-1;
        tt = t;
    }
    else
    {
        while (i < n-1 && d[i]/d[n-1] <= t)
            i++;
        tt = t-d[i-1]/d[n-1];
    }
    
    // Interpolate between these two states
    RealVectorStateSpace::interpolate(stateList[i > 0 ? i-1 : 0], stateList[i], tt, state);
    delete [] d;
    
    // Set the correct chart, guessing it might be one of the adjacent charts first
    StateType *astate = state->as<StateType>();
    const Eigen::VectorXd x = astate->toVector();
    const AtlasChart &c1 = stateList[i > 0 ? i-1 : 0]->getChart();
    const AtlasChart &c2 = stateList[i]->getChart();
    if (c1.inP(c1.psiInverse(x)))
        astate->setChart(c1);
    else if (c2.inP(c2.psiInverse(x)))
        astate->setChart(c2);
    else
    {
        const AtlasChart *c = owningChart(x);
        if (!c)
            c = &newChart(x);
        astate->setChart(*c);
    }
}

bool ompl::base::AtlasStateSpace::hasSymmetricInterpolate (void) const
{
    if (noAtlas_)
        return RealVectorStateSpace::hasSymmetricInterpolate();
    return false;
}

void ompl::base::AtlasStateSpace::copyState (State *destination, const State *source) const
{
    RealVectorStateSpace::copyState(destination, source);
    destination->as<StateType>()->setChart(source->as<StateType>()->getChart());
}

ompl::base::StateSamplerPtr ompl::base::AtlasStateSpace::allocDefaultStateSampler (void) const
{
    if (noAtlas_)
        return RealVectorStateSpace::allocDefaultStateSampler();
    return StateSamplerPtr(new AtlasStateSampler(*this));
}

ompl::base::State *ompl::base::AtlasStateSpace::allocState (void) const
{
    return new StateType(n_);
}

void ompl::base::AtlasStateSpace::freeState (State *state) const
{
    StateType *const astate = state->as<StateType>();
    const AtlasChart *const c = astate->getChart_safe();
    if (c)
        c->disown(astate);
    delete astate;
}

/// Protected

Eigen::MatrixXd ompl::base::AtlasStateSpace::numericalJacobian (const Eigen::VectorXd &x) const
{
    Eigen::VectorXd y1 = x;
    Eigen::VectorXd y2 = x;
    Eigen::MatrixXd jac(n_-k_, n_);
    
    // Use a 7-point central difference stencil on each entire column at once
    for (std::size_t j = 0; j < n_; j++)
    {
        // Make step size as small as possible while still giving usable accuracy
        const double h = std::sqrt(std::numeric_limits<double>::epsilon()) * (x[j] >= 1 ? x[j] : 1);
        
        y1[j] += h; y2[j] -= h;
        const Eigen::VectorXd m1 = (bigF(y1) - bigF(y2)) / (y1[j]-y2[j]);   // Can't assume y1[j]-y2[j] == 2*h because of precision errors
        y1[j] += h; y2[j] -= h;
        const Eigen::VectorXd m2 = (bigF(y1) - bigF(y2)) / (y1[j]-y2[j]);
        y1[j] += h; y2[j] -= h;
        const Eigen::VectorXd m3 = (bigF(y1) - bigF(y2)) / (y1[j]-y2[j]);
        
        jac.col(j) = 1.5*m1 - 0.6*m2 + 0.1*m3;
        
        // Reset for next iteration
        y1[j] = y2[j] = x[j];
    }
    
    return jac;
}
