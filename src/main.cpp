#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s)
{
	auto found_null = s.find("null");
	auto b1 = s.find_first_of("[");
	auto b2 = s.find_first_of("}");
	if (found_null != string::npos)
	{
		return "";
	}
	else if (b1 != string::npos && b2 != string::npos)
	{
		return s.substr(b1, b2 - b1 + 2);
	}
	return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}

int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}
	}
	return closestWaypoint;
}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2((map_y-y),(map_x-x));

	double angle = fabs(theta-heading);
	angle = min(2*pi() - angle, angle);

	if (angle > pi()/4)
	{
		closestWaypoint++;
		if (closestWaypoint == maps_x.size())
		{
			closestWaypoint = 0;
		}
	}
	return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};
}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};
}

/**
 * @brief this is a computational context containing useful constants and actual status of a car
 */
struct context_t
{
	// Status
	int lane = 1;                            //!< starting car lane
	double speed = 0.;                       //!< starting car speed

	// Useful consts
	enum lane_status
	{
		CAR_CLOSE_AHEAD = 0,
		CAR_TO_THE_LEFT = 1,
		CAR_TO_THE_RIGHT = 2,
	};

	const double SAFE_GAP = 30.;             //!< safe gap (in both directions) to come to line
	const double MAX_SPEED = 49.5;           //!< maximum speed in [mph]
	const double MAX_ACC = 0.224;            //!< maximum acceleration (to not exeed jerk limit)
	const double LANE_WIDTH = 4.;            //!< lane width
	const double PREDICTION_BASE_STEP = 30.; //!< base step of prediction (used for spline construction)
	const int N_PREDICTION_POINTS = 50;      //!< number of prediction points
	const double UPDATE_RATE = 0.02;         //!< update rate
};

/**
 * @brief function analyzes traffic and produces vector of statuses of what lanes can be used
 */
vector<bool> checkLanes(context_t &ctx, vector<vector<double>> sensor_fusion, double my_car_s, int prediction_length)
{
	bool car_close_ahead = false;
	bool car_to_the_left = false;
	bool car_to_the_right = false;

	for (const auto &car : sensor_fusion)
	{
		double d = car[6];
		int car_lane = -1;
		if (d >= 0 && d < ctx.LANE_WIDTH)
			car_lane = 0;
		else if (d >= ctx.LANE_WIDTH && d < 2 * ctx.LANE_WIDTH)
			car_lane = 1;
		else if (d >= 2 * ctx.LANE_WIDTH && d <= 3 * ctx.LANE_WIDTH)
			car_lane = 2;

		if (car_lane < 0 || car_lane > 2)
			continue;//--^

		double car_vx = car[3];
		double car_vy = car[4];
		double car_speed = distance(0., 0., car_vx, car_vy);
		double car_s = car[5];

		// Extrapolate car position using speed
		car_s += prediction_length * ctx.UPDATE_RATE * car_speed;

		// Other car is in our lane
		if (ctx.lane - car_lane == 0)
		{
			// Gap is unsafe (forward direction)
			car_close_ahead |= ((car_s - my_car_s) > 0.) && ((car_s - my_car_s) < ctx.SAFE_GAP);
		}
		// Other car is to the left
		else if (ctx.lane - car_lane == 1)
		{
			// Gap is unsafe (both directions)
			car_to_the_left |= ((car_s - my_car_s) > -ctx.SAFE_GAP) && ((car_s - my_car_s) < ctx.SAFE_GAP);

		}
		// Other car is to the right
		else if (ctx.lane - car_lane == -1)
		{
			// Gap is unsafe (both directions)
			car_to_the_right |= ((car_s - my_car_s) > -ctx.SAFE_GAP) && ((car_s - my_car_s) < ctx.SAFE_GAP);
		}
	}

	return {car_close_ahead, car_to_the_left, car_to_the_right};
}

/**
 * @brief function determines car behaviour (lane/speed) given traffic information
 */
void chooseBehaviour(context_t &ctx, vector<bool> lanes_status)
{
	// We have a car ahead of us decide if we want to change lane a slow down
	if (lanes_status[context_t::CAR_CLOSE_AHEAD])
	{
		// We assume that chnge to the left is easier than chnge to right so check left first
		if (!lanes_status[context_t::CAR_TO_THE_LEFT] && ctx.lane > 0)
		{
			// Shifting to the left lane
			--ctx.lane;
		}
		// Less desirible behaviour is shifting to the right lane
		else if (!lanes_status[context_t::CAR_TO_THE_RIGHT] && ctx.lane < 2)
		{
			// Shifting to the right lane
			++ctx.lane;
		}
		// Nowhere to change line to, so choose the least desirable behaviour - slowing down
		else
		{
			// Slowing down in smooth manner to not exceed jerk
			ctx.speed -= ctx.MAX_ACC;
		}
	}
	// Line ahead of us is safe try to optimize our position on the road
	else
	{
		// We are not on the center lane and can change to it (do so to optimize our possible future lane shifts)
		if ((ctx.lane == 2 && !lanes_status[context_t::CAR_TO_THE_LEFT]) ||
			(ctx.lane == 0 && !lanes_status[context_t::CAR_TO_THE_RIGHT]))
		{
			ctx.lane = 1;
		}

		// If the speed is not near limit can encrease it
		if (ctx.speed < ctx.MAX_SPEED)
		{
			ctx.speed += ctx.MAX_ACC;
		}
	}
	return;
}

/**
 * @brief function produces base points for spline (to change splines smoothly from epoch to epoch). Produces
 *        values that help switch from global to car coordinate system at given epoch
 */
vector<vector<double>> buildPointsForBaseSpline(context_t &ctx, double car_x, double car_y, double car_yaw, double car_s, 
							double &ref_x, double &ref_y, double &ref_yaw,
							const vector<double> &prev_path_x, const vector<double> &prev_path_y,
							vector<double> mwp_s, vector<double> mwp_x, vector<double> mwp_y)
{
	vector<double> ptsx;
	vector<double> ptsy;

	// Reference coordinate system
	ref_x = car_x;
	ref_y = car_y;
	ref_yaw = deg2rad(car_yaw);

	int prev_size = prev_path_x.size();
	// Can not operate with previous path so use car values for reference
	if (prev_size < 2)
	{
		// Emulate previous point using yaw
		ptsx.emplace_back(car_x - cos(car_yaw));
		ptsx.emplace_back(car_x);

		ptsy.emplace_back(car_y - sin(car_yaw));
		ptsy.emplace_back(car_y);
	}
	// Use the previous path's endpoint as starting reference (for smooth transition)
	else
	{
		// Last point
		ref_x = prev_path_x[prev_size - 1];
		ref_y = prev_path_y[prev_size - 1];

		// Point before last
		double ref_x_prev = prev_path_x[prev_size - 2];
		double ref_y_prev = prev_path_y[prev_size - 2];

		ref_yaw = atan2(ref_y - ref_y_prev, ref_x - ref_x_prev);

		// Use two points that make the path tangent to the path's previous endpoint
		ptsx.emplace_back(ref_x_prev);
		ptsx.emplace_back(ref_x);

		ptsy.emplace_back(ref_y_prev);
		ptsy.emplace_back(ref_y);
	}

	// Using Frenet coordinates adding points with step of 30 m
	vector<double> wp0 = getXY(car_s + 1. *ctx.PREDICTION_BASE_STEP, (ctx.lane + 0.5) * ctx.LANE_WIDTH, mwp_s, mwp_x, mwp_y);
	vector<double> wp1 = getXY(car_s + 2. *ctx.PREDICTION_BASE_STEP, (ctx.lane + 0.5) * ctx.LANE_WIDTH, mwp_s, mwp_x, mwp_y);
	vector<double> wp2 = getXY(car_s + 3. *ctx.PREDICTION_BASE_STEP, (ctx.lane + 0.5) * ctx.LANE_WIDTH, mwp_s, mwp_x, mwp_y);

	ptsx.emplace_back(wp0[0]);
	ptsx.emplace_back(wp1[0]);
	ptsx.emplace_back(wp2[0]);

	ptsy.emplace_back(wp0[1]);
	ptsy.emplace_back(wp1[1]);
	ptsy.emplace_back(wp2[1]);

	// Convert points to reference (car's) coordinate system
	for (int i = 0; i < ptsx.size(); ++i)
	{
		double shift_x = ptsx[i] - ref_x;
		double shift_y = ptsy[i] - ref_y;

		ptsx[i] = (shift_x * cos(0 - ref_yaw) - shift_y * sin(0 - ref_yaw));
		ptsy[i] = (shift_x * sin(0 - ref_yaw) + shift_y * cos(0 - ref_yaw));
	}

	return {ptsx, ptsy};
}

/**
 * @brief function produces points with constant step on given spline
 */
void fillRestOfPath(context_t &ctx, vector<double> &next_x_vals, vector<double> &next_y_vals, tk::spline &base_spline, double ref_x, double ref_y, double ref_yaw, int prediction_length)
{
	// Use prediction step and distribute rest of the points uniformly
	double base_step_x = ctx.PREDICTION_BASE_STEP;
	double base_step_y = base_spline(base_step_x);
	double base_step_dist = distance(0.,0., base_step_x, base_step_y);

	double x_to_add = 0.;
	double steps = base_step_dist / (ctx.UPDATE_RATE * ctx.speed / 2.24);

	for (int i = 0; i < ctx.N_PREDICTION_POINTS - prediction_length; ++i)
	{
		double x_point = x_to_add + base_step_x / steps;
		double y_point = base_spline(x_point);

		x_to_add = x_point;

		double x_car = x_point;
		double y_car = y_point;

		// Swith back to global coordinate system
		x_point = (x_car * cos(ref_yaw) - y_car * sin(ref_yaw));
		y_point = (x_car * sin(ref_yaw) + y_car * cos(ref_yaw));

		x_point += ref_x;
		y_point += ref_y;

		next_x_vals.emplace_back(x_point);
		next_y_vals.emplace_back(y_point);
	}
	return;
}


int main()
{
	uWS::Hub h;

	// Load up map values for waypoint's x,y,s and d normalized normal vectors
	vector<double> map_waypoints_x;
	vector<double> map_waypoints_y;
	vector<double> map_waypoints_s;
	vector<double> map_waypoints_dx;
	vector<double> map_waypoints_dy;

	// Waypoint map to read from
	string map_file_ = "../data/highway_map.csv";
	// The max s value before wrapping around the track back to 0
	double max_s = 6945.554;

	ifstream in_map_(map_file_.c_str(), ifstream::in);

	string line;
	while (getline(in_map_, line))
	{
		istringstream iss(line);
		double x;
		double y;
		float s;
		float d_x;
		float d_y;
		iss >> x;
		iss >> y;
		iss >> s;
		iss >> d_x;
		iss >> d_y;
		map_waypoints_x.push_back(x);
		map_waypoints_y.push_back(y);
		map_waypoints_s.push_back(s);
		map_waypoints_dx.push_back(d_x);
		map_waypoints_dy.push_back(d_y);
	}

	context_t ctx; //our car context
	h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy,&ctx](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length, uWS::OpCode opCode)
	{
		// "42" at the start of the message means there's a websocket message event.
		// The 4 signifies a websocket message
		// The 2 signifies a websocket event

		//auto sdata = string(data).substr(0, length);
		//cout << sdata << endl;
		if (length && length > 2 && data[0] == '4' && data[1] == '2')
		{
			auto s = hasData(data);

			if (s != "")
			{
				auto j = json::parse(s);
				string event = j[0].get<string>();

				if (event == "telemetry")
				{
					// j[1] is the data JSON object

					// Main car's localization Data
					double car_x = j[1]["x"];
					double car_y = j[1]["y"];
					double car_s = j[1]["s"];
					double car_d = j[1]["d"];
					double car_yaw = j[1]["yaw"];
					double car_speed = j[1]["speed"];

					// Previous path data given to the Planner
					auto previous_path_x = j[1]["previous_path_x"];
					auto previous_path_y = j[1]["previous_path_y"];
					// Previous path's end s and d values
					double end_path_s = j[1]["end_path_s"];
					double end_path_d = j[1]["end_path_d"];

					// Sensor Fusion Data, a list of all other cars on the same side of the road.
					auto sensor_fusion = j[1]["sensor_fusion"];

					int prediction_length = previous_path_x.size();
					if (prediction_length > 0) car_s = end_path_s;

					// Check lanes for other cars
					vector<bool> lanes_status = checkLanes(ctx, sensor_fusion, car_s, prediction_length);

					// Choose desired behaviour according to it
					chooseBehaviour(ctx, lanes_status);

					// Create a list of waypoints with step of 30m
					double ref_x = 0.;
					double ref_y = 0.;
					double ref_yaw = 0.;
					vector<vector<double>> pts = buildPointsForBaseSpline(ctx, car_x, car_y, car_yaw, car_s, ref_x, ref_y, ref_yaw,
									previous_path_x, previous_path_y, map_waypoints_s, map_waypoints_x, map_waypoints_y);

					// Create a base spline given points
					tk::spline base_spline;
					base_spline.set_points(pts[0], pts[1]);

					// Define the actual (x,y) points we will use for the planner
					vector<double> next_x_vals;
					vector<double> next_y_vals;

					// Use all points we did not use last time
					for (int i = 0; i < previous_path_x.size(); ++i)
					{
						next_x_vals.emplace_back(previous_path_x[i]);
						next_y_vals.emplace_back(previous_path_y[i]);
					}

					// Add points
					fillRestOfPath(ctx, next_x_vals, next_y_vals, base_spline, ref_x, ref_y, ref_yaw, prediction_length);

					json msgJson;

					// TODO: define a path made up of (x,y) points that the car will visit sequentially every .02 seconds
					msgJson["next_x"] = next_x_vals;
					msgJson["next_y"] = next_y_vals;

					auto msg = "42[\"control\","+ msgJson.dump()+"]";

					//this_thread::sleep_for(chrono::milliseconds(1000));
					ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
				}
			}
			else
			{
				// Manual driving
				std::string msg = "42[\"manual\",{}]";
				ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
			}
		}
	});

	// We don't need this since we're not using HTTP but if it's removed the
	// program doesn't compile :-(
	h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data, size_t, size_t)
	{
		const std::string s = "<h1>Hello world!</h1>";
		if (req.getUrl().valueLength == 1)
		{
			res->end(s.data(), s.length());
		}
		else
		{
			// i guess this should be done more gracefully?
			res->end(nullptr, 0);
		}
	});

	h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req)
	{
		std::cout << "Connected!!!" << std::endl;
	});

	h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code, char *message, size_t length)
	{
		ws.close();
		std::cout << "Disconnected" << std::endl;
	});

	int port = 4567;
	if (h.listen(port))
	{
		std::cout << "Listening to port " << port << std::endl;
	}
	else
	{
		std::cerr << "Failed to listen to port" << std::endl;
		return -1;
	}

	h.run();
	return 0;
}
