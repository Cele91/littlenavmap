/*****************************************************************************
* Copyright 2015-2017 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "route/route.h"

#include "geo/calculations.h"
#include "common/maptools.h"
#include "common/unit.h"
#include "route/flightplanentrybuilder.h"
#include "common/procedurequery.h"

#include <QRegularExpression>

#include <marble/GeoDataLineString.h>

using atools::geo::Pos;
using atools::geo::Line;
using atools::geo::LineString;
using atools::geo::nmToMeter;
using atools::geo::normalizeCourse;
using atools::geo::meterToNm;
using atools::geo::manhattanDistance;

Route::Route()
{
  resetActive();
}

Route::Route(const Route& other)
  : QList<RouteLeg>(other)
{
  copy(other);
}

Route::~Route()
{

}

Route& Route::operator=(const Route& other)
{
  copy(other);

  return *this;
}

void Route::resetActive()
{
  activeLegResult.distanceFrom1 = activeLegResult.distanceFrom2 = activeLegResult.distance =
                                                                    map::INVALID_DISTANCE_VALUE;
  activeLegResult.status = atools::geo::INVALID;
  activePos = map::PosCourse();
  activeLeg = map::INVALID_INDEX_VALUE;
}

void Route::copy(const Route& other)
{
  clear();
  append(other);

  totalDistance = other.totalDistance;
  flightplan = other.flightplan;
  shownTypes = other.shownTypes;
  boundingRect = other.boundingRect;
  activePos = other.activePos;
  trueCourse = other.trueCourse;

  arrivalLegs = other.arrivalLegs;
  starLegs = other.starLegs;
  departureLegs = other.departureLegs;

  departureLegsOffset = other.departureLegsOffset;
  starLegsOffset = other.starLegsOffset;
  arrivalLegsOffset = other.arrivalLegsOffset;

  activeLeg = other.activeLeg;
  activeLegResult = other.activeLegResult;

  // Update flightplan pointers to this instance
  for(RouteLeg& routeLeg : *this)
    routeLeg.setFlightplan(&flightplan);

}

int Route::getNextUserWaypointNumber() const
{
  /* Get number from user waypoint from user defined waypoint in fs flight plan */
  static const QRegularExpression USER_WP_ID("^WP([0-9]+)$");

  int nextNum = 0;

  for(const atools::fs::pln::FlightplanEntry& entry : flightplan.getEntries())
  {
    if(entry.getWaypointType() == atools::fs::pln::entry::USER)
      nextNum = std::max(QString(USER_WP_ID.match(entry.getWaypointId()).captured(1)).toInt(), nextNum);
  }
  return nextNum + 1;
}

bool Route::canEditLeg(int index) const
{
  // Do not allow any edits between the procedures

  if(hasAnyDepartureProcedure() && index < departureLegsOffset + departureLegs.size())
    return false;

  if(hasAnyStarProcedure() && index > starLegsOffset)
    return false;

  if(hasAnyArrivalProcedure() && index > arrivalLegsOffset)
    return false;

  return true;
}

bool Route::canEditPoint(int index) const
{
  return at(index).isRoute();
}

void Route::updateActiveLegAndPos()
{
  updateActiveLegAndPos(activePos);
}

/* Compare crosstrack distance fuzzy */
bool Route::isSmaller(const atools::geo::LineDistance& dist1, const atools::geo::LineDistance& dist2,
                      float epsilon)
{
  return std::abs(dist1.distance) < std::abs(dist2.distance) + epsilon;
}

void Route::updateActiveLegAndPos(const map::PosCourse& pos)
{
  if(isEmpty() || !pos.isValid())
  {
    resetActive();
    return;
  }

  if(activeLeg == map::INVALID_INDEX_VALUE)
  {
    // Start with nearest leg
    float crossDummy;
    nearestAllLegIndex(pos, crossDummy, activeLeg);
  }

  if(activeLeg >= size())
    activeLeg = size() - 1;

  activePos = pos;

  if(size() == 1)
  {
    // Special case point route
    activeLeg = 0;
    // Test if still nearby
    activePos.pos.distanceMeterToLine(first().getPosition(), first().getPosition(), activeLegResult);
  }
  else
  {
    if(activeLeg == 0)
      // Reset from point route
      activeLeg = 1;

    activePos.pos.distanceMeterToLine(getPositionAt(activeLeg - 1), getPositionAt(activeLeg), activeLegResult);
  }

  // Get potential next leg and course difference
  int nextLeg = activeLeg + 1;
  float courseDiff = 0.f;
  if(nextLeg < size())
  {
    // Catch the case of initial fixes or others that are points instead of lines and try the next legs
    if(!at(activeLeg).getProcedureLeg().isHold())
    {
      while(atools::contains(at(nextLeg).getProcedureLegType(),
                             {proc::INITIAL_FIX, proc::START_OF_PROCEDURE}) &&
            getPositionAt(nextLeg - 1) == getPositionAt(nextLeg) &&
            nextLeg < size() - 2)
        nextLeg++;
    }
    else
    {
      // Jump all initial fixes for holds since the next line can probably not overlap
      while(atools::contains(at(nextLeg).getProcedureLegType(),
                             {proc::INITIAL_FIX, proc::START_OF_PROCEDURE}) && nextLeg < size() - 2)
        nextLeg++;
    }
    Pos pos1 = getPositionAt(nextLeg - 1);
    Pos pos2 = getPositionAt(nextLeg);

    // Calculate course difference
    float legCrs = normalizeCourse(pos1.angleDegTo(pos2));
    courseDiff = atools::mod(pos.course - legCrs + 360.f, 360.f);
    if(courseDiff > 180.f)
      courseDiff = 360.f - courseDiff;

    // qDebug() << "ACTIVE" << at(activeLeg);
    // qDebug() << "NEXT" << at(nextLeg);

    // Test next leg
    atools::geo::LineDistance nextLegResult;
    activePos.pos.distanceMeterToLine(pos1, pos2, nextLegResult);
    // qDebug() << "NEXT" << nextLeg << nextLegResult;

    bool switchToNextLeg = false;
    if(at(activeLeg).getProcedureLeg().isHold())
    {
      // qDebug() << "ACTIVE HOLD";
      // Test next leg if we can exit a hold
      if(at(nextLeg).getProcedureLeg().line.getPos1() == at(activeLeg).getPosition())
      {
        // qDebug() << "HOLD SAME";

        // hold point is the same as next leg starting point
        if(nextLegResult.status == atools::geo::ALONG_TRACK && // on track of next
           std::abs(nextLegResult.distance) < nmToMeter(0.5f) && // not too far away from start of next
           nextLegResult.distanceFrom1 > nmToMeter(0.75f) && // Travelled some distance into the new segment
           courseDiff < 25.f) // Keep course
          switchToNextLeg = true;
      }
      else
      {
        atools::geo::LineDistance resultHold;
        at(activeLeg).getProcedureLeg().holdLine.distanceMeterToLine(activePos.pos, resultHold);
        // qDebug() << "NEXT HOLD" << nextLeg << resultHold;

        // qDebug() << "HOLD DIFFER";
        // Hold point differs from next leg start - use the helping line
        if(resultHold.status == atools::geo::ALONG_TRACK && // Check if we are outside of the hold
           resultHold.distance < nmToMeter(at(activeLeg).getProcedureLeg().turnDirection == "R" ? -0.5f : 0.5f))
          switchToNextLeg = true;
      }
    }
    else
    {
      if(at(nextLeg).getProcedureLeg().isHold())
      {
        // Ignore all other rulues and use distance to hold point to activate hold
        if(std::abs(nextLegResult.distance) < nmToMeter(0.5f))
          switchToNextLeg = true;
      }
      else if(at(activeLeg).getProcedureLegType() == proc::PROCEDURE_TURN)
      {
        // Ignore the after end indication for current leg for procedure turns since turn can happen earlier
        if(isSmaller(nextLegResult, activeLegResult, 100.f /* meter */) && courseDiff < 45.f)
          switchToNextLeg = true;
      }
      else
      {
        // Check if we can advance to the next leg - if at end of current and distance to next is smaller and course similar
        if(activeLegResult.status == atools::geo::AFTER_END ||
           (isSmaller(nextLegResult, activeLegResult, 10.f /* meter */) && courseDiff < 90.f))
          switchToNextLeg = true;
      }
    }

    if(switchToNextLeg)
    {
      // qDebug() << "Switching to next leg";
      // Either left current leg or closer to next and on courses
      // Do not track on missed if legs are not displayed
      if(!(!(shownTypes & map::MISSED_APPROACH) && at(nextLeg).getProcedureLeg().isMissed()))
      {
        // Go to next leg and increase all values
        activeLeg = nextLeg;
        pos.pos.distanceMeterToLine(getPositionAt(activeLeg - 1), getPositionAt(activeLeg), activeLegResult);
      }
    }
  }
  // qDebug() << "active" << activeLeg << "size" << size() << "ident" << at(activeLeg).getIdent() <<
  // maptypes::approachLegTypeFullStr(at(activeLeg).getProcedureLeg().type);
}

bool Route::getRouteDistances(float *distFromStart, float *distToDest,
                              float *nextLegDistance, float *crossTrackDistance) const
{
  const proc::MapProcedureLeg *geometryLeg = nullptr;

  if(activeLeg == map::INVALID_INDEX_VALUE)
    return false;

  if(at(activeLeg).isAnyProcedure() && (at(activeLeg).getGeometry().size() > 2))
    // Use arc or intercept geometry to calculate distance
    geometryLeg = &at(activeLeg).getProcedureLeg();

  if(crossTrackDistance != nullptr)
  {
    if(geometryLeg != nullptr)
    {
      atools::geo::LineDistance lineDist;
      geometryLeg->geometry.distanceMeterToLineString(activePos.pos, lineDist);
      if(lineDist.status == atools::geo::ALONG_TRACK)
        *crossTrackDistance = meterToNm(lineDist.distance);
      else
        *crossTrackDistance = map::INVALID_DISTANCE_VALUE;
    }
    else if(activeLegResult.status == atools::geo::ALONG_TRACK)
      *crossTrackDistance = meterToNm(activeLegResult.distance);
    else
      *crossTrackDistance = map::INVALID_DISTANCE_VALUE;
  }

  int routeIndex = activeLeg;
  if(routeIndex != map::INVALID_INDEX_VALUE)
  {
    if(routeIndex >= size())
      routeIndex = size() - 1;

    float distToCurrent = 0.f;

    bool activeIsMissed = at(activeLeg).getProcedureLeg().isMissed();

    // Ignore missed approach legs until the active is a missed approach leg
    if(!at(routeIndex).getProcedureLeg().isMissed() || activeIsMissed)
    {
      if(geometryLeg != nullptr)
      {
        atools::geo::LineDistance result;
        geometryLeg->geometry.distanceMeterToLineString(activePos.pos, result);
        distToCurrent = meterToNm(result.distanceFrom2);
      }
      else
        distToCurrent = meterToNm(getPositionAt(routeIndex).distanceMeterTo(activePos.pos));
    }

    if(nextLegDistance != nullptr)
      *nextLegDistance = distToCurrent;

    // Sum up all distances along the legs
    // Ignore missed approach legs until the active is a missedd approach leg
    float fromstart = 0.f;
    for(int i = 0; i <= routeIndex; i++)
    {
      if(!at(i).getProcedureLeg().isMissed() || activeIsMissed)
        fromstart += at(i).getDistanceTo();
      else
        break;
    }
    fromstart -= distToCurrent;
    fromstart = std::abs(fromstart);

    if(distFromStart != nullptr)
      *distFromStart = std::max(fromstart, 0.f);

    if(distToDest != nullptr)
    {
      if(!activeIsMissed)
        *distToDest = std::max(totalDistance - fromstart, 0.f);
      else
      {
        // Summarize remaining missed leg distance if on missed
        *distToDest = 0.f;
        for(int i = routeIndex + 1; i < size(); i++)
        {
          if(at(i).getProcedureLeg().isMissed())
            *distToDest += at(i).getDistanceTo();
        }
        *distToDest += distToCurrent;
        *distToDest = std::abs(*distToDest);
      }
    }

    return true;
  }
  return false;
}

float Route::getDistanceFromStart(const atools::geo::Pos& pos) const
{
  atools::geo::LineDistance result;
  int leg = getNearestRouteLegResult(pos, result, false /* ignoreNotEditable */);
  float distFromStart = map::INVALID_DISTANCE_VALUE;

  if(leg < map::INVALID_INDEX_VALUE && result.status == atools::geo::ALONG_TRACK)
  {
    float fromstart = 0.f;
    for(int i = 1; i < leg; i++)
    {
      if(!at(i).getProcedureLeg().isMissed())
        fromstart += nmToMeter(at(i).getDistanceTo());
      else
        break;
    }
    fromstart += result.distanceFrom1;
    fromstart = std::abs(fromstart);

    distFromStart = std::max(fromstart, 0.f);
  }
  return meterToNm(distFromStart);
}

float Route::getTopOfDescentFromStart() const
{
  if(!isEmpty())
    return getTotalDistance() - getTopOfDescentFromDestination();

  return 0.f;
}

float Route::getTopOfDescentFromDestination() const
{
  if(!isEmpty())
  {
    float cruisingAltitude = Unit::rev(getFlightplan().getCruisingAltitude(), Unit::altFeetF);
    float diff = (cruisingAltitude - last().getPosition().getAltitude());

    // Either nm per 1000 something alt or km per 1000 something alt
    float distNm = Unit::rev(OptionData::instance().getRouteTodRule(), Unit::distNmF);
    float altFt = Unit::rev(1000.f, Unit::altFeetF);

    return diff / altFt * distNm;
  }
  return 0.f;
}

atools::geo::Pos Route::getTopOfDescent() const
{
  if(!isEmpty())
    return positionAtDistance(getTopOfDescentFromStart());

  return atools::geo::EMPTY_POS;
}

atools::geo::Pos Route::positionAtDistance(float distFromStartNm) const
{
  if(distFromStartNm < 0.f || distFromStartNm > totalDistance)
    return atools::geo::EMPTY_POS;

  atools::geo::Pos retval;

  // Find the leg that contains the given distance point
  float total = 0.f;
  int foundIndex = map::INVALID_INDEX_VALUE; // Found leg is from this index to index + 1
  for(int i = 0; i < size() - 1; i++)
  {
    total += at(i + 1).getDistanceTo();
    if(total > distFromStartNm)
    {
      // Distance already within leg
      foundIndex = i;
      break;
    }
  }

  if(foundIndex < size() - 1)
  {
    foundIndex++;
    if(at(foundIndex).getGeometry().size() > 2)
    {
      // Use approach geometry to display
      float base = distFromStartNm - (total - at(foundIndex).getProcedureLeg().calculatedDistance);
      float fraction = base / at(foundIndex).getProcedureLeg().calculatedDistance;
      retval = at(foundIndex).getGeometry().interpolate(fraction);
    }
    else
    {
      float base = distFromStartNm - (total - at(foundIndex).getDistanceTo());
      float fraction = base / at(foundIndex).getDistanceTo();
      retval = getPositionAt(foundIndex - 1).interpolate(getPositionAt(foundIndex), fraction);
    }
  }

  return retval;
}

void Route::getNearest(const CoordinateConverter& conv, int xs, int ys, int screenDistance,
                       map::MapSearchResult& mapobjects, QList<proc::MapProcedurePoint>& procPoints,
                       bool includeProcedure) const
{
  using maptools::insertSortedByDistance;

  int x, y;

  for(int i = 0; i < size(); i++)
  {
    const RouteLeg& leg = at(i);
    if(!includeProcedure && leg.isAnyProcedure())
      // Do not edit procedures
      continue;

    if(conv.wToS(leg.getPosition(), x, y) && manhattanDistance(x, y, xs, ys) < screenDistance)
    {
      if(leg.getVor().isValid())
      {
        map::MapVor vor = leg.getVor();
        vor.routeIndex = i;
        insertSortedByDistance(conv, mapobjects.vors, &mapobjects.vorIds, xs, ys, vor);
      }

      if(leg.getWaypoint().isValid())
      {
        map::MapWaypoint wp = leg.getWaypoint();
        wp.routeIndex = i;
        insertSortedByDistance(conv, mapobjects.waypoints, &mapobjects.waypointIds, xs, ys, wp);
      }

      if(leg.getNdb().isValid())
      {
        map::MapNdb ndb = leg.getNdb();
        ndb.routeIndex = i;
        insertSortedByDistance(conv, mapobjects.ndbs, &mapobjects.ndbIds, xs, ys, ndb);
      }

      if(leg.getAirport().isValid())
      {
        map::MapAirport ap = leg.getAirport();
        ap.routeIndex = i;
        insertSortedByDistance(conv, mapobjects.airports, &mapobjects.airportIds, xs, ys, ap);
      }

      if(leg.getMapObjectType() == map::INVALID)
      {
        map::MapUserpoint up;
        up.routeIndex = i;
        up.name = leg.getIdent() + " (not found)";
        up.position = leg.getPosition();
        mapobjects.userPoints.append(up);
      }

      if(leg.getMapObjectType() == map::USER)
      {
        map::MapUserpoint up;
        up.id = i;
        up.routeIndex = i;
        up.name = leg.getIdent();
        up.position = leg.getPosition();
        mapobjects.userPoints.append(up);
      }

      if(leg.isAnyProcedure())
        procPoints.append(proc::MapProcedurePoint(leg.getProcedureLeg()));
    }
  }
}

bool Route::hasDepartureParking() const
{
  if(hasValidDeparture())
    return first().getDepartureParking().isValid();

  return false;
}

bool Route::hasDepartureHelipad() const
{
  if(hasDepartureStart())
    return first().getDepartureStart().helipadNumber > 0;

  return false;
}

bool Route::hasDepartureStart() const
{
  if(hasValidDeparture())
    return first().getDepartureStart().isValid();

  return false;
}

bool Route::isFlightplanEmpty() const
{
  return getFlightplan().isEmpty();
}

bool Route::hasValidDeparture() const
{
  return !getFlightplan().isEmpty() &&
         getFlightplan().getEntries().first().getWaypointType() == atools::fs::pln::entry::AIRPORT &&
         first().isValid();
}

bool Route::hasValidDestination() const
{
  return !getFlightplan().isEmpty() &&
         getFlightplan().getEntries().last().getWaypointType() == atools::fs::pln::entry::AIRPORT &&
         last().isValid();
}

bool Route::hasEntries() const
{
  return getFlightplan().getEntries().size() > 2;
}

bool Route::canCalcRoute() const
{
  return getFlightplan().getEntries().size() >= 2;
}

void Route::clearAllProcedures()
{
  clearProcedures(proc::PROCEDURE_ALL);
}

void Route::clearProcedures(proc::MapProcedureTypes type)
{
  // Clear procedure legs
  if(type & proc::PROCEDURE_SID)
    departureLegs.clearApproach();
  if(type & proc::PROCEDURE_SID_TRANSITION)
    departureLegs.clearTransition();

  if(type & proc::PROCEDURE_STAR_TRANSITION)
    starLegs.clearTransition();
  if(type & proc::PROCEDURE_STAR)
    starLegs.clearApproach();

  if(type & proc::PROCEDURE_TRANSITION)
    arrivalLegs.clearTransition();
  if(type & proc::PROCEDURE_APPROACH)
    arrivalLegs.clearApproach();

  // Remove properties from flight plan
  clearFlightplanProcedureProperties(type);

  // Remove legs from flight plan and route legs
  eraseProcedureLegs(type);
  updateAll();
}

void Route::clearFlightplanProcedureProperties(proc::MapProcedureTypes type)
{
  ProcedureQuery::clearFlightplanProcedureProperties(flightplan.getProperties(), type);
}

void Route::updateProcedureLegs(FlightplanEntryBuilder *entryBuilder)
{
  eraseProcedureLegs(proc::PROCEDURE_ALL);

  departureLegsOffset = map::INVALID_INDEX_VALUE;
  starLegsOffset = map::INVALID_INDEX_VALUE;
  arrivalLegsOffset = map::INVALID_INDEX_VALUE;

  // Create route legs and flight plan entries from departure
  if(!departureLegs.isEmpty())
    // Starts always after departure airport
    departureLegsOffset = 1;

  QList<atools::fs::pln::FlightplanEntry>& entries = flightplan.getEntries();
  for(int i = 0; i < departureLegs.size(); i++)
  {
    int insertIndex = 1 + i;
    RouteLeg obj(&flightplan);
    obj.createFromApproachLeg(i, departureLegs, &at(i));
    insert(insertIndex, obj);

    atools::fs::pln::FlightplanEntry entry;
    entryBuilder->buildFlightplanEntry(departureLegs.at(insertIndex - 1), entry, true);
    entries.insert(insertIndex, entry);
  }

  // Create route legs and flight plan entries from STAR
  if(!starLegs.isEmpty())
    starLegsOffset = size() - 1;

  for(int i = 0; i < starLegs.size(); i++)
  {
    const RouteLeg *prev = size() >= 2 ? &at(size() - 2) : nullptr;

    RouteLeg obj(&flightplan);
    obj.createFromApproachLeg(i, starLegs, prev);
    insert(size() - 1, obj);

    atools::fs::pln::FlightplanEntry entry;
    entryBuilder->buildFlightplanEntry(starLegs.at(i), entry, true);
    entries.insert(entries.size() - 1, entry);
  }

  // Create route legs and flight plan entries from arrival
  if(!arrivalLegs.isEmpty())
    arrivalLegsOffset = size() - 1;

  for(int i = 0; i < arrivalLegs.size(); i++)
  {
    const RouteLeg *prev = size() >= 2 ? &at(size() - 2) : nullptr;

    RouteLeg obj(&flightplan);
    obj.createFromApproachLeg(i, arrivalLegs, prev);
    insert(size() - 1, obj);

    atools::fs::pln::FlightplanEntry entry;
    entryBuilder->buildFlightplanEntry(arrivalLegs.at(i), entry, true);
    entries.insert(entries.size() - 1, entry);
  }

  // Leave procedure information in the PLN file
  clearFlightplanProcedureProperties(proc::PROCEDURE_ALL);

  ProcedureQuery::extractLegsForFlightplanProperties(flightplan.getProperties(), arrivalLegs, starLegs, departureLegs);
}

void Route::eraseProcedureLegs(proc::MapProcedureTypes type)
{
  QVector<int> indexes;

  // Collect indexes to delete in reverse order
  for(int i = size() - 1; i >= 0; i--)
  {
    const RouteLeg& routeLeg = at(i);
    if(type & routeLeg.getProcedureLeg().mapType) // Check if any bits/flags overlap
      indexes.append(i);
  }

  // Delete in route legs and flight plan from the end
  for(int i = 0; i < indexes.size(); i++)
  {
    removeAt(indexes.at(i));
    flightplan.getEntries().removeAt(indexes.at(i));
  }
}

void Route::updateAll()
{
  updateIndicesAndOffsets();
  updateMagvar();
  updateDistancesAndCourse();
  updateBoundingRect();
}

void Route::updateIndicesAndOffsets()
{
  if(activeLeg < map::INVALID_INDEX_VALUE)
  {
    // Put the active back into bounds
    activeLeg = std::min(activeLeg, size() - 1);
    activeLeg = std::max(activeLeg, 0);
  }

  departureLegsOffset = map::INVALID_INDEX_VALUE;
  starLegsOffset = map::INVALID_INDEX_VALUE;
  arrivalLegsOffset = map::INVALID_INDEX_VALUE;

  // Update offsets
  for(int i = 0; i < size(); i++)
  {
    RouteLeg& leg = (*this)[i];
    leg.setFlightplanEntryIndex(i);

    if(leg.getProcedureLeg().isAnyDeparture() && departureLegsOffset == map::INVALID_INDEX_VALUE)
      departureLegsOffset = i;

    if(leg.getProcedureLeg().isAnyStar() && starLegsOffset == map::INVALID_INDEX_VALUE)
      starLegsOffset = i;

    if(leg.getProcedureLeg().isArrival() && arrivalLegsOffset == map::INVALID_INDEX_VALUE)
      arrivalLegsOffset = i;
  }
}

const RouteLeg *Route::getActiveLegCorrected(bool *corrected) const
{
  int idx = getActiveLegIndexCorrected(corrected);

  if(idx != map::INVALID_INDEX_VALUE)
    return &at(idx);
  else
    return nullptr;
}

const RouteLeg *Route::getActiveLeg() const
{
  if(activeLeg != map::INVALID_INDEX_VALUE)
    return &at(activeLeg);
  else
    return nullptr;
}

int Route::getActiveLegIndexCorrected(bool *corrected) const
{
  if(activeLeg == map::INVALID_INDEX_VALUE)
    return map::INVALID_INDEX_VALUE;

  int nextLeg = activeLeg + 1;
  if(nextLeg < size() && nextLeg == size() &&
     at(nextLeg).isAnyProcedure() /* && (at(nextLeg).getProcedureLeg().type == maptypes::INITIAL_FIX ||
                                   *    at(nextLeg).isHold())*/)
  {
    if(corrected != nullptr)
      *corrected = true;
    return activeLeg + 1;
  }
  else
  {
    if(corrected != nullptr)
      *corrected = false;
    return activeLeg;
  }
}

bool Route::isActiveMissed() const
{
  const RouteLeg *leg = getActiveLeg();
  if(leg != nullptr)
    return leg->getProcedureLeg().isMissed();
  else
    return false;
}

bool Route::isPassedLastLeg() const
{
  if((activeLeg >= size() - 1 || (activeLeg + 1 < size() && at(activeLeg + 1).getProcedureLeg().isMissed())) &&
     activeLegResult.status == atools::geo::AFTER_END)
    return true;

  return false;
}

void Route::setActiveLeg(int value)
{
  if(value > 0 && value < size())
    activeLeg = value;
  else
    activeLeg = 1;

  activePos.pos.distanceMeterToLine(at(activeLeg - 1).getPosition(), at(activeLeg).getPosition(), activeLegResult);
}

bool Route::isAirportAfterArrival(int index)
{
  return (hasAnyArrivalProcedure() /*|| hasStarProcedure()*/) &&
         index == size() - 1 && at(index).getMapObjectType() == map::AIRPORT;
}

void Route::updateDistancesAndCourse()
{
  totalDistance = 0.f;
  RouteLeg *last = nullptr;
  for(int i = 0; i < size(); i++)
  {
    if(isAirportAfterArrival(i))
      break;

    RouteLeg& leg = (*this)[i];
    leg.updateDistanceAndCourse(i, last);
    if(!leg.getProcedureLeg().isMissed())
      totalDistance += leg.getDistanceTo();
    last = &leg;
  }
}

void Route::updateMagvar()
{
  // get magvar from internal database objects (waypoints, VOR and others)
  for(int i = 0; i < size(); i++)
    (*this)[i].updateMagvar();

  // Update missing magvar values using neighbour entries
  for(int i = 0; i < size(); i++)
    (*this)[i].updateInvalidMagvar(i, this);

  trueCourse = true;
  // Check if there is any magnetic variance on the route
  // If not (all user waypoints) use true heading
  for(const RouteLeg& obj : *this)
  {
    // Route contains correct magvar if any of these objects were found
    if(obj.getMapObjectType() & map::NAV_MAGVAR)
    {
      trueCourse = false;
      break;
    }
  }
}

/* Update the bounding rect using marble functions to catch anti meridian overlap */
void Route::updateBoundingRect()
{
  Marble::GeoDataLineString line;

  for(const RouteLeg& routeLeg : *this)
    line.append(Marble::GeoDataCoordinates(routeLeg.getPosition().getLonX(),
                                           routeLeg.getPosition().getLatY(), 0., Marble::GeoDataCoordinates::Degree));

  Marble::GeoDataLatLonBox box = Marble::GeoDataLatLonBox::fromLineString(line);
  boundingRect = atools::geo::Rect(box.west(), box.north(), box.east(), box.south());
  boundingRect.toDeg();
}

void Route::nearestAllLegIndex(const map::PosCourse& pos, float& crossTrackDistanceMeter,
                               int& index) const
{
  crossTrackDistanceMeter = map::INVALID_DISTANCE_VALUE;
  index = map::INVALID_INDEX_VALUE;

  if(!pos.isValid())
    return;

  float minDistance = map::INVALID_DISTANCE_VALUE;

  // Check only until the approach starts if required
  atools::geo::LineDistance result;

  for(int i = 1; i < size(); i++)
  {
    pos.pos.distanceMeterToLine(getPositionAt(i - 1), getPositionAt(i), result);
    float distance = std::abs(result.distance);

    if(result.status != atools::geo::INVALID && distance < minDistance)
    {
      minDistance = distance;
      crossTrackDistanceMeter = result.distance;
      index = i;
    }
  }

  if(crossTrackDistanceMeter < map::INVALID_DISTANCE_VALUE)
  {
    if(std::abs(crossTrackDistanceMeter) > atools::geo::nmToMeter(100.f))
    {
      // Too far away from any segment or point
      crossTrackDistanceMeter = map::INVALID_DISTANCE_VALUE;
      index = map::INVALID_INDEX_VALUE;
    }
  }
}

int Route::getNearestRouteLegResult(const atools::geo::Pos& pos,
                                    atools::geo::LineDistance& lineDistanceResult, bool ignoreNotEditable) const
{
  int index = map::INVALID_INDEX_VALUE;
  lineDistanceResult.status = atools::geo::INVALID;
  lineDistanceResult.distance = map::INVALID_DISTANCE_VALUE;

  if(!pos.isValid())
    return index;

  // Check only until the approach starts if required
  atools::geo::LineDistance result, minResult;
  minResult.status = atools::geo::INVALID;
  minResult.distance = map::INVALID_DISTANCE_VALUE;

  for(int i = 1; i < size(); i++)
  {
    if(ignoreNotEditable && !canEditLeg(i))
      continue;

    pos.distanceMeterToLine(getPositionAt(i - 1), getPositionAt(i), result);

    if(result.status != atools::geo::INVALID && std::abs(result.distance) < std::abs(minResult.distance))
    {
      minResult = result;
      index = i;
    }
  }

  if(index != map::INVALID_INDEX_VALUE)
    lineDistanceResult = minResult;

  return index;
}

QDebug operator<<(QDebug out, const Route& route)
{
  out << "Route ======================" << endl;
  out << "Departure ======================" << endl;
  out << "offset" << route.getDepartureLegsOffset() << endl;
  out << route.getDepartureLegs() << endl;
  out << "STAR ======================" << endl;
  out << "offset" << route.getStarLegsOffset() << endl;
  out << route.getStarLegs() << endl;
  out << "Arrival ======================" << endl;
  out << "offset" << route.getArrivalLegsOffset() << endl;
  out << route.getArrivalLegs() << endl;

  for(int i = 0; i < route.size(); ++i)
    out << i << route.at(i) << endl;
  out << "======================" << endl;

  return out;
}