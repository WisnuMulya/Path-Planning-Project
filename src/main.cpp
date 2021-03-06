#include <uWS/uWS.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "helpers.h"
#include "json.hpp"
#include "spline.h"

// for convenience
using nlohmann::json;
using std::string;
using std::vector;

int main() {
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

  std::ifstream in_map_(map_file_.c_str(), std::ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    std::istringstream iss(line);
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
  
  // Ego car starts at lane 1
  int lane = 1;

  // Set reference speed for the car
  double ref_velocity = 0.0; // MPH

  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,
               &map_waypoints_dx,&map_waypoints_dy,&lane,&ref_velocity]
              (uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
               uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
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

          // Sensor Fusion Data, a list of all other cars on the same side 
          //   of the road.
          auto sensor_fusion = j[1]["sensor_fusion"];

          json msgJson;
 
          /**
           * TODO: define a path made up of (x,y) points that the car will visit
           *   sequentially every .02 seconds
           */
	  // Previous path size
	  int previous_path_size = previous_path_x.size();

	  // Set target velocity
	  double target_velocity = 49.5;

	  // Check if there's a car too close to the ego car and to its left and right
	  if (previous_path_size > 0) {
	    car_s = end_path_s;
	  }
	  
	  bool is_too_close = false;
	  bool is_car_left = false;
	  bool is_car_right = false;
	  double sf_s, sf_d, sf_vx, sf_vy, sf_velocity, s_future;

	  for (int i = 0; i < sensor_fusion.size(); i++) {
	    // Grab sensor fusion measurement of a car
	    sf_vx = sensor_fusion[i][3];
	    sf_vy = sensor_fusion[i][4];
	    sf_velocity = sqrt(pow(sf_vx, 2) + pow(sf_vy, 2));
	    sf_s = sensor_fusion[i][5];
	    sf_d = sensor_fusion[i][6];
	    s_future = sf_s + ((double)previous_path_size * .02 * sf_velocity);
	      
	    // Check if a car is in ego car's lane
	    if ((sf_d < 2 + (lane * 4) + 2) && (sf_d > 2 + (lane * 4) - 2)) {
	      // Check if a car's future position would be too close to the ego car's end path waypoint
	      if ((s_future > car_s) && (s_future - car_s < 30)) {
		is_too_close = true;
	      }
	    }

	    // Check if there's a car on the left lane
	    else if ((sf_d < lane * 4) && (sf_d > (lane * 4) - 4)) {
	      // Check if the car on the left lane prevents ego car to change lane
	      // (whether the car is 20m in front or behind the ego car).
	      if (abs(s_future - car_s) < 20) {
		is_car_left = true;
	      }
	    }

	    // Check if there's a car on the right lane
	    else if ((sf_d < (lane * 4) + 8) && (sf_d > (lane * 4) + 4)) {
	      // Check if the car on the right lane prevents ego car to change lane
	      // (whether the car is 20m in front or behind the ego car)
	      if (abs(s_future - car_s) < 20) {
		is_car_right = true;
	      }
	    }
	  }

	  // Change lane if there's a car too close and it's safe
	  if (is_too_close) {
	    // If it's safe to turn left and the ego is not on the leftmost lane
	    if (!is_car_left && (lane != 0)) {
	      lane -= 1;
	    }
	    // If it's safe to turn right and the ego is not on the rightmost lane
	    else if (!is_car_right && (lane != 2)) {
	      lane += 1;
	    }
	  }

	  
	  // Define anchor points for appending  waypoints to the next path
	  vector<double> anchor_points_x, anchor_points_y;

	  // Define reference points as either where the car is or the end point of previous path
	  double ref_x = car_x;
	  double ref_y = car_y;
	  double ref_yaw = deg2rad(car_yaw);

	  // If previous path is almost empty, set car's location as reference
	  if (previous_path_size < 2) {
	    double previous_car_x = car_x - cos(car_yaw);
	    double previous_car_y = car_y - sin(car_yaw);

	    anchor_points_x.push_back(previous_car_x);
	    anchor_points_x.push_back(car_x);

	    anchor_points_y.push_back(previous_car_y);
	    anchor_points_y.push_back(car_y);
	  } else {
	    ref_x = previous_path_x[previous_path_size - 1];
	    ref_y = previous_path_y[previous_path_size - 1];

	    double ref_x_previous = previous_path_x[previous_path_size - 2];
	    double ref_y_previous = previous_path_y[previous_path_size - 2];
	    ref_yaw = atan2(ref_y - ref_y_previous, ref_x - ref_x_previous);

	    anchor_points_x.push_back(ref_x_previous);
	    anchor_points_x.push_back(ref_x);

	    anchor_points_y.push_back(ref_y_previous);
	    anchor_points_y.push_back(ref_y);
	  }

	  // Append anchor waypoints with 30m space in between them
	  vector<double> anchor_wp0 = getXY(car_s + 30, (2 + 4 * lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
	  vector<double> anchor_wp1 = getXY(car_s + 60, (2 + 4 * lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
	  vector<double> anchor_wp2 = getXY(car_s + 90, (2 + 4 * lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);

	  anchor_points_x.push_back(anchor_wp0[0]);
	  anchor_points_x.push_back(anchor_wp1[0]);
	  anchor_points_x.push_back(anchor_wp2[0]);

	  anchor_points_y.push_back(anchor_wp0[1]);
	  anchor_points_y.push_back(anchor_wp1[1]);
	  anchor_points_y.push_back(anchor_wp2[1]);

	  // Transform anchor points' basis, so the car is at origin and at 0 degree yaw
	  double shift_x, shift_y;
	  for (int i = 0; i < anchor_points_x.size(); i++) {
	    shift_x = anchor_points_x[i] - ref_x;
	    shift_y = anchor_points_y[i] - ref_y;

	    anchor_points_x[i] = shift_x * cos(-ref_yaw) - shift_y * sin(-ref_yaw);
	    anchor_points_y[i] = shift_x * sin(-ref_yaw) + shift_y * cos(-ref_yaw);
	  }
	  
	  // Create a spline
	  tk::spline spline;
	  
	  // Set points to the spline
	  spline.set_points(anchor_points_x, anchor_points_y);

	  // The car's path
	  vector<double> next_x_vals;
          vector<double> next_y_vals;

	  // Append all previous path to the next path
	  for (int i = 0; i < previous_path_size; i++) {
	    next_x_vals.push_back(previous_path_x[i]);
	    next_y_vals.push_back(previous_path_y[i]);
	  }

	  // Calculate the number of points the spline would be splitted to achieve the target velocity
	  double target_x = 30.0;
	  double target_y = spline(target_x);
	  double target_distance = sqrt(pow(target_x, 2) + pow(target_y, 2));

	  // Define x_add_on as waypoint x to add in the basis of car's at origin with yaw 0
	  double x_add_on = 0;

	  // Adding waypoints to the next path
	  double next_x, next_y, N;
	  
	  for (int i = 0; i < 50 - previous_path_size; ++i) {
	    // Slows down if there's a car too close or speeds up otherwise
	    if (is_too_close) {
	      ref_velocity -= .224;
	    } else if (ref_velocity < target_velocity) {
	      ref_velocity += .224;
	    }

	    N = target_distance / (0.02 * (ref_velocity / 2.24));
	    next_x = x_add_on + (target_x / N);
	    next_y = spline(next_x);

	    x_add_on = next_x;

	    // Transform next X and Y to the basis of the map
	    shift_x = next_x;
	    shift_y = next_y;

	    next_x = ref_x + (shift_x * cos(ref_yaw) - shift_y * sin(ref_yaw));
	    next_y = ref_y + (shift_x * sin(ref_yaw) + shift_y * cos(ref_yaw));
	    
	    next_x_vals.push_back(next_x);
	    next_y_vals.push_back(next_y);
	  }
	  // END

          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }  // end "telemetry" if
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }  // end websocket if
  }); // end h.onMessage

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  
  h.run();
}
