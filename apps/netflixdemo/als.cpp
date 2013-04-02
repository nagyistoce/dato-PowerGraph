/**
 * Copyright (c) 2009 Carnegie Mellon University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */


/**
 * \file
 *
 * \brief The main file for the ALS matrix factorization algorithm.
 *
 * This file contains the main body of the ALS matrix factorization
 * algorithm.
 */

#include <Eigen/Dense>

#include <boost/config/warning_disable.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_stl.hpp>
#include <boost/algorithm/string/predicate.hpp>




// This file defines the serialization code for the eigen types.
#include "eigen_serialization.hpp"

#include <graphlab.hpp>
#include <graphlab/util/stl_util.hpp>
#include "stats.hpp"

#include <graphlab/macros_def.hpp>

const int SAFE_NEG_OFFSET = 2; //add 2 to negative node id
//to prevent -0 and -1 which arenot allowed


bool NMFHACK = false;

/**
 * \brief We use the eigen library's vector type to represent
 * mathematical vectors.
 */
typedef Eigen::VectorXd vec_type;

/**
 * \brief We use the eigen library's matrix type to represent
 * matrices.
 */
typedef Eigen::MatrixXd mat_type;




/**
 * \ingroup toolkit_matrix_factorization
 *
 * \brief the vertex data type which contains the latent factor.
 *
 * Each row and each column in the matrix corresponds to a different
 * vertex in the ALS graph.  Associated with each vertex is a factor
 * (vector) of latent parameters that represent that vertex.  The goal
 * of the ALS algorithm is to find the values for these latent
 * parameters such that the non-zero entries in the matrix can be
 * predicted by taking the dot product of the row and column factors.
 */
struct vertex_data {
  /**
   * \brief A shared "constant" that specifies the number of latent
   * values to use.
   */
  static size_t NLATENT;
  /** \brief The number of times this vertex has been updated. */
  uint32_t nupdates;
  /** \brief The most recent L1 change in the factor value */
  float residual; //! how much the latent value has changed
  /** \brief The latent factor for this vertex */
  vec_type factor;
  /**
   * \brief Simple default constructor which randomizes the vertex
   *  data
   */
  vertex_data() : nupdates(0), residual(1) { randomize(); }
  /** \brief Randomizes the latent factor */
  void randomize() { factor.resize(NLATENT); factor.setRandom(); }
  /** \brief Save the vertex data to a binary archive */
  void save(graphlab::oarchive& arc) const {
    arc << nupdates << residual << factor;
  }
  /** \brief Load the vertex data from a binary archive */
  void load(graphlab::iarchive& arc) {
    arc >> nupdates >> residual >> factor;
  }
}; // end of vertex data


size_t vertex_data::NLATENT = 20;

/**
 * \brief The edge data stores the entry in the matrix.
 *
 * In addition the edge data also stores the most recent error estimate.
 */
struct edge_data : public graphlab::IS_POD_TYPE {
  /**
   * \brief The type of data on the edge;
   *
   * \li *Train:* the observed value is correct and used in training
   * \li *Validate:* the observed value is correct but not used in training
   * \li *Predict:* The observed value is not correct and should not be
   *        used in training.
   */
  enum data_role_type { TRAIN, VALIDATE, PREDICT  };

  /** \brief the observed value for the edge */
  float obs;

  /** \brief The train/validation/test designation of the edge */
  data_role_type role;

  /** \brief basic initialization */
  edge_data(float obs = 0, data_role_type role = TRAIN) :
    obs(obs), role(role) { }

}; // end of edge data


/**
 * \brief The graph type is defined in terms of the vertex and edge
 * data.
 */
typedef graphlab::distributed_graph<vertex_data, edge_data> graph_type;

#include "implicit.hpp"

stats_info count_edges(const graph_type::edge_type & edge){
  stats_info ret;

  if (edge.data().role == edge_data::TRAIN)
     ret.training_edges = 1;
  else if (edge.data().role == edge_data::VALIDATE)
     ret.validation_edges = 1;
  ret.max_user = (size_t)edge.source().id();
  ret.max_item = (-edge.target().id()-SAFE_NEG_OFFSET);
  return ret;
}



/**
 * \brief Given a vertex and an edge return the other vertex in the
 * edge.
 */
inline graph_type::vertex_type
get_other_vertex(graph_type::edge_type& edge,
                 const graph_type::vertex_type& vertex) {
  return vertex.id() == edge.source().id()? edge.target() : edge.source();
}; // end of get_other_vertex




/**
 * \brief The gather type used to construct XtX and Xty needed for the ALS
 * update
 *
 * To compute the ALS update we need to compute the sum of
 * \code
 *  sum: XtX = nbr.factor.transpose() * nbr.factor
 *  sum: Xy  = nbr.factor * edge.obs
 * \endcode
 * For each of the neighbors of a vertex.
 *
 * To do this in the Gather-Apply-Scatter model the gather function
 * computes and returns a pair consisting of XtX and Xy which are then
 * added. The gather type represents that tuple and provides the
 * necessary gather_type::operator+= operation.
 *
 */
class gather_type {
public:
  /**
   * \brief Stores the current sum of nbr.factor.transpose() *
   * nbr.factor
   */
  mat_type XtX;

  /**
   * \brief Stores the current sum of nbr.factor * edge.obs
   */
  vec_type Xy;

  /** \brief basic default constructor */
  gather_type() { }

  /**
   * \brief This constructor computes XtX and Xy and stores the result
   * in XtX and Xy
   */
  gather_type(const vec_type& X, const double y) :
    XtX(X.size(), X.size()), Xy(X.size()) {
    XtX.triangularView<Eigen::Upper>() = X * X.transpose();
    Xy = X * y;
  } // end of constructor for gather type

  /** \brief Save the values to a binary archive */
  void save(graphlab::oarchive& arc) const { arc << XtX << Xy; }

  /** \brief Read the values from a binary archive */
  void load(graphlab::iarchive& arc) { arc >> XtX >> Xy; }

  /**
   * \brief Computes XtX += other.XtX and Xy += other.Xy updating this
   * tuples value
   */
  gather_type& operator+=(const gather_type& other) {
    if(other.Xy.size() == 0) {
      ASSERT_EQ(other.XtX.rows(), 0);
      ASSERT_EQ(other.XtX.cols(), 0);
    } else {
      if(Xy.size() == 0) {
        ASSERT_EQ(XtX.rows(), 0);
        ASSERT_EQ(XtX.cols(), 0);
        XtX = other.XtX; Xy = other.Xy;
      } else {
        XtX.triangularView<Eigen::Upper>() += other.XtX;
        Xy += other.Xy;
      }
    }
    return *this;
  } // end of operator+=

}; // end of gather type



/**
 * \brief ALS vertex program implements the alternating least squares
 * algorithm in the Gather-Apply-Scatter abstraction.
 *
 * The ALS update treats adjacent vertices (rows or columns) as "X"
 * (independent) values and the edges (matrix entries) as observed "y"
 * (dependent) values and then updates the current vertex value as a
 * weight "w" such that:
 *
 *    y = X * w + noise
 *
 * This is accomplished using the following equation:
 *
 *    w = inv(X' * X) * (X' * y)
 *
 * We implement this in the Gather-Apply-Scatter model by:
 *
 *  1) Gather: returns the tuple (X' * X, X' * y)
 *     Sum:   (aX' * aX, aX * ay) + (bX' * bX, bX * by) =
 *                 (aX' * aX + bX' * bX, aX * ay + bX * by)
 *
 *  2) Apply: Solves  inv(X' * X) * (X' * y)
 *
 *  3) Scatter: schedules the update of adjacent vertices if this
 *      vertex has changed sufficiently and the edge is not well
 *      predicted.
 *
 *
 */
class als_vertex_program :
  public graphlab::ivertex_program<graph_type, gather_type,
                                   graphlab::messages::sum_priority>,
  public graphlab::IS_POD_TYPE {
public:
  /** The convergence tolerance */
  static double TOLERANCE;
  static double LAMBDA;
  static size_t MAX_UPDATES;
  static double MAXVAL;
  static double MINVAL;
  static int    REGNORMAL; //regularization type

  /** The set of edges to gather along */
  edge_dir_type gather_edges(icontext_type& context,
                             const vertex_type& vertex) const {
    return graphlab::ALL_EDGES;
  }; // end of gather_edges

  /** The gather function computes XtX and Xy */
  gather_type gather(icontext_type& context, const vertex_type& vertex,
                     edge_type& edge) const {
    if(edge.data().role == edge_data::TRAIN) {
      const vertex_type other_vertex = get_other_vertex(edge, vertex);
      return gather_type(other_vertex.data().factor, edge.data().obs);
    } else return gather_type();
  } // end of gather function

  /** apply collects the sum of XtX and Xy */
  void apply(icontext_type& context, vertex_type& vertex,
             const gather_type& sum) {
    // Get and reset the vertex data
    vertex_data& vdata = vertex.data();
    // Determine the number of neighbors.  Each vertex has only in or
    // out edges depending on which side of the graph it is located
    if(sum.Xy.size() == 0) { vdata.residual = 0; ++vdata.nupdates; return; }
    mat_type XtX = sum.XtX;
    vec_type Xy = sum.Xy;
    // Add regularization
    double regularization = LAMBDA;
    if (REGNORMAL)
      regularization = LAMBDA*vertex.num_out_edges();
    for(int i = 0; i < XtX.rows(); ++i)
      XtX(i,i) += regularization;
    // Solve the least squares problem using eigen ----------------------------
    const vec_type old_factor = vdata.factor;
    vdata.factor = XtX.selfadjointView<Eigen::Upper>().ldlt().solve(Xy);
    // Compute the residual change in the factor factor -----------------------
    if (NMFHACK) {
      for (size_t i = 0;i < vertex_data::NLATENT; ++i) {
        if (vdata.factor(i) < 0) vdata.factor(i) = 0;
      }
    }
    vdata.residual = (vdata.factor - old_factor).cwiseAbs().sum() / XtX.rows();
    ++vdata.nupdates;
  } // end of apply

  /** The edges to scatter along */
  edge_dir_type scatter_edges(icontext_type& context,
                              const vertex_type& vertex) const {
    return graphlab::ALL_EDGES;
  }; // end of scatter edges

  /** Scatter reschedules neighbors */
  void scatter(icontext_type& context, const vertex_type& vertex,
               edge_type& edge) const {
    edge_data& edata = edge.data();
    if(edata.role == edge_data::TRAIN) {
      const vertex_type other_vertex = get_other_vertex(edge, vertex);
      const vertex_data& vdata = vertex.data();
      const vertex_data& other_vdata = other_vertex.data();
      const double pred = vdata.factor.dot(other_vdata.factor);
      const float error = std::fabs(edata.obs - pred);
      const double priority = (error * vdata.residual);
      // Reschedule neighbors ------------------------------------------------
      if( priority > TOLERANCE && other_vdata.nupdates < MAX_UPDATES)
        context.signal(other_vertex, priority);
    }
  } // end of scatter function


  /**
   * \brief Signal all vertices on one side of the bipartite graph
   */
  static graphlab::empty signal_left(icontext_type& context,
                                     const vertex_type& vertex) {
    if(vertex.num_out_edges() > 0) context.signal(vertex);
    return graphlab::empty();
  } // end of signal_left



}; // end of als vertex program



/**
 * \brief The graph loader function is a line parser used for
 * distributed graph construction.
 */
inline bool graph_loader(graph_type& graph,
                         const std::string& filename,
                         const std::string& line) {
  ASSERT_FALSE(line.empty());
  if (boost::starts_with(line, "#") || boost::starts_with(line, "%")) {
    logstream(LOG_INFO) << line << std::endl;
    return true;
  }
  namespace qi = boost::spirit::qi;
  namespace ascii = boost::spirit::ascii;
  namespace phoenix = boost::phoenix;
  // Determine the role of the data
  edge_data::data_role_type role = edge_data::TRAIN;
  if(boost::ends_with(filename,".validate")) role = edge_data::VALIDATE;
  else if(boost::ends_with(filename, ".predict")) role = edge_data::PREDICT;
  // Parse the line
  graph_type::vertex_id_type source_id(-1), target_id(-1);
  float obs(0);
  const bool success = qi::phrase_parse
    (line.begin(), line.end(),
     //  Begin grammar
     (
      qi::ulong_[phoenix::ref(source_id) = qi::_1] >> -qi::char_(',') >>
      qi::ulong_[phoenix::ref(target_id) = qi::_1] >>
      -(-qi::char_(',') >> qi::float_[phoenix::ref(obs) = qi::_1])
      )
     ,
     //  End grammar
     ascii::space);

  if(!success) return false;

  if(role == edge_data::TRAIN || role == edge_data::VALIDATE){
    if (obs < als_vertex_program::MINVAL || obs > als_vertex_program::MAXVAL)
      logstream(LOG_FATAL)<<"Rating values should be between " << als_vertex_program::MINVAL << " and " << als_vertex_program::MAXVAL << ". Got value: " << obs << " [ user: " << source_id << " to item: " <<target_id << " ] " << std::endl;
  }

  // map target id into a separate number space
  target_id = -(graphlab::vertex_id_type(target_id + SAFE_NEG_OFFSET));
  // Create an edge and add it to the graph
  graph.add_edge(source_id, target_id, edge_data(obs, role));
  return true; // successful load
} // end of graph_loader



/**
 * \brief Given an edge compute the error associated with that edge
 */
double extract_l2_error(const graph_type::edge_type & edge) {
  double pred =
    edge.source().data().factor.dot(edge.target().data().factor);
  pred = std::min(als_vertex_program::MAXVAL, pred);
  pred = std::max(als_vertex_program::MINVAL, pred);
  return (edge.data().obs - pred) * (edge.data().obs - pred);
} // end of extract_l2_error



double als_vertex_program::TOLERANCE = 1e-3;
double als_vertex_program::LAMBDA = 0.01;
size_t als_vertex_program::MAX_UPDATES = -1;
double als_vertex_program::MAXVAL = 1e+100;
double als_vertex_program::MINVAL = -1e+100;
int    als_vertex_program::REGNORMAL = 1;





/**
 * \brief The error aggregator is used to accumulate the overal
 * prediction error.
 *
 * The error aggregator is itself a "reduction type" and contains the
 * two static methods "map" and "finalize" which operate on
 * error_aggregators and are used by the engine.add_edge_aggregator
 * api.
 */
struct error_aggregator : public graphlab::IS_POD_TYPE {
  typedef als_vertex_program::icontext_type icontext_type;
  typedef graph_type::edge_type edge_type;
  double train_error, validation_error;
  error_aggregator() :
    train_error(0), validation_error(0) { }
  error_aggregator& operator+=(const error_aggregator& other) {
    train_error += other.train_error;
    validation_error += other.validation_error;
    return *this;
  }
  static error_aggregator map(icontext_type& context, const graph_type::edge_type& edge) {
    error_aggregator agg;
    if(edge.data().role == edge_data::TRAIN) {
      agg.train_error = extract_l2_error(edge);
    } else if(edge.data().role == edge_data::VALIDATE) {
      agg.validation_error = extract_l2_error(edge);
    }
    return agg;
  }
  static void finalize(icontext_type& context, const error_aggregator& agg) {
    const double train_error = std::sqrt(agg.train_error / info.training_edges);
    context.cout() << "Time in seconds: " << context.elapsed_seconds() << "\tiTraining RMSE: " << train_error;
    if(info.validation_edges > 0) {
      const double validation_error =
        std::sqrt(agg.validation_error / info.validation_edges);
      context.cout() << "\tValidation RMSE: " << validation_error;
    }
    context.cout() << std::endl;
  }
}; // end of error aggregator




/**
 * \brief The prediction saver is used by the graph.save routine to
 * output the final predictions back to the filesystem.
 */
struct prediction_saver {
  typedef graph_type::vertex_type vertex_type;
  typedef graph_type::edge_type   edge_type;
  std::string save_vertex(const vertex_type& vertex) const {
    return ""; //nop
  }
  std::string save_edge(const edge_type& edge) const {
    if(edge.data().role == edge_data::PREDICT) {
      std::stringstream strm;
      const double prediction =
        edge.source().data().factor.dot(edge.target().data().factor);
      strm << edge.source().id() << '\t';
      strm << (-edge.target().id() - SAFE_NEG_OFFSET) << '\t';
      strm << prediction << '\n';
      return strm.str();
    } else return "";
  }
}; // end of prediction_saver


struct linear_model_saver_U {
  typedef graph_type::vertex_type vertex_type;
  typedef graph_type::edge_type   edge_type;
  /* save the linear model, using the format:
     nodeid) factor1 factor2 ... factorNLATENT \n
  */
  std::string save_vertex(const vertex_type& vertex) const {
    if (vertex.num_out_edges() > 0){
      std::string ret = boost::lexical_cast<std::string>(vertex.id()) + " ";
      for (uint i=0; i< vertex_data::NLATENT; i++)
        ret += boost::lexical_cast<std::string>(vertex.data().factor[i]) + " ";
        ret += "\n";
      return ret;
    }
    else return "";
  }
  std::string save_edge(const edge_type& edge) const {
    return "";
  }
};

struct linear_model_saver_V {
  typedef graph_type::vertex_type vertex_type;
  typedef graph_type::edge_type   edge_type;
  /* save the linear model, using the format:
     nodeid) factor1 factor2 ... factorNLATENT \n
  */
  std::string save_vertex(const vertex_type& vertex) const {
    if (vertex.num_out_edges() == 0){
      std::string ret = boost::lexical_cast<std::string>(-vertex.id()-SAFE_NEG_OFFSET) + ") ";
      for (uint i=0; i< vertex_data::NLATENT; i++)
        ret += boost::lexical_cast<std::string>(vertex.data().factor[i]) + " ";
        ret += "\n";
      return ret;
    }
    else return "";
  }
  std::string save_edge(const edge_type& edge) const {
    return "";
  }
};

struct map_join {
  boost::unordered_map<graphlab::vertex_id_type, double> data; // rating than movie
  void save(graphlab::oarchive& oarc) const {
    oarc << data;
  }
  void load(graphlab::iarchive& iarc) {
    iarc >> data;
  }

  map_join& operator+=(const map_join& other) {
    std::copy(other.data.begin(), other.data.end(),
              std::inserter(data, data.end()));
    return *this;
  }

  std::vector<std::pair<double, graphlab::vertex_id_type> > get_top_k(size_t n) {
    std::vector<std::pair<double, graphlab::vertex_id_type> > ret;
    boost::unordered_map<graphlab::vertex_id_type, double>::const_iterator iter = data.begin();
    std::vector<std::pair<double, graphlab::vertex_id_type> > all_copy;
    while (iter != data.end()) {
      all_copy.push_back(std::make_pair(iter->second, iter->first));
      ++iter;
    }
    std::sort(all_copy.rbegin(), all_copy.rend());
    size_t limit = all_copy.size() < 10 ? all_copy.size() : 10;
    std::copy(all_copy.begin(), all_copy.begin() + limit,
              std::inserter(ret, ret.end()));
    return ret;
  }
};

/**
 * \brief The engine type used by the ALS matrix factorization
 * algorithm.
 *
 * The ALS matrix factorization algorithm currently uses the
 * synchronous engine.  However we plan to add support for alternative
 * engines in the future.
 */
typedef graphlab::omni_engine<als_vertex_program> engine_type;

int main(int argc, char** argv) {
  global_logger().set_log_level(LOG_INFO);
  global_logger().set_log_to_console(true);

  // Parse command line options -----------------------------------------------
  const std::string description =
    "Compute the ALS factorization of a matrix.";
  graphlab::command_line_options clopts(description);
  std::string input_dir;
  std::string predictions;
  size_t interval = 10;
  std::string movielist_dir;
  std::string exec_type = "synchronous";
  clopts.attach_option("matrix", input_dir,
                       "The directory containing the matrix file");
  clopts.add_positional("matrix");
  clopts.attach_option("D",  vertex_data::NLATENT,
                       "Number of latent parameters to use.");
  clopts.attach_option("max_iter", als_vertex_program::MAX_UPDATES,
                       "The maxumum number of udpates allowed for a vertex");
  clopts.attach_option("lambda", als_vertex_program::LAMBDA,
                       "ALS regularization weight");
  clopts.attach_option("tol", als_vertex_program::TOLERANCE,
                       "residual termination threshold");
  clopts.attach_option("maxval", als_vertex_program::MAXVAL, "max allowed value");
  clopts.attach_option("minval", als_vertex_program::MINVAL, "min allowed value");
  clopts.attach_option("interval", interval,
                       "The time in seconds between error reports");
  clopts.attach_option("predictions", predictions,
                       "The prefix (folder and filename) to save predictions.");
  clopts.attach_option("engine", exec_type,
                       "The engine type synchronous or asynchronous");
  clopts.attach_option("regnormal", als_vertex_program::REGNORMAL,
                       "regularization type. 1 = weighted according to neighbors num. 0 = no weighting - just lambda");
  clopts.attach_option("movielist", movielist_dir,
                       "Movie List");
  clopts.attach_option("nmfhack", NMFHACK,
                       "NMF Hack");


  parse_implicit_command_line(clopts);

  if(!clopts.parse(argc, argv) || input_dir == "") {
    std::cout << "Error in parsing command line arguments." << std::endl;
    clopts.print_description();
    return EXIT_FAILURE;
  }


  boost::unordered_map<graph_type::vertex_id_type, std::string> mlist;
  if (!movielist_dir.empty()) {
    std::ifstream fin(movielist_dir.c_str());
    size_t id = 1;
    while(fin.good()) {
      std::string name;
      std::getline(fin, name);
      graphlab::vertex_id_type gid = -(graphlab::vertex_id_type(id + SAFE_NEG_OFFSET));
      mlist[gid] = name;
      id++;
    }
    fin.close();
  }



  ///! Initialize control plain using mpi
  graphlab::mpi_tools::init(argc, argv);
  graphlab::distributed_control dc;

  dc.cout() << "Loading graph." << std::endl;
  graphlab::timer timer;
  graph_type graph(dc, clopts);
  graph.load(input_dir, graph_loader);
  dc.cout() << "Loading graph. Finished in "
            << timer.current_time() << std::endl;

  if (dc.procid() == 0)
    add_implicit_edges<edge_data>(implicitratingtype, graph, dc);

  dc.cout() << "Finalizing graph." << std::endl;
  timer.start();
   graph.finalize();
  dc.cout() << "Finalizing graph. Finished in "
            << timer.current_time() << std::endl;


  dc.cout()
      << "========== Graph statistics on proc " << dc.procid()
      << " ==============="
      << "\n Num vertices: " << graph.num_vertices()
      << "\n Num edges: " << graph.num_edges()
      << "\n Num replica: " << graph.num_replicas()
      << "\n Replica to vertex ratio: "
      << float(graph.num_replicas())/graph.num_vertices()
      << "\n --------------------------------------------"
      << "\n Num local own vertices: " << graph.num_local_own_vertices()
      << "\n Num local vertices: " << graph.num_local_vertices()
      << "\n Replica to own ratio: "
      << (float)graph.num_local_vertices()/graph.num_local_own_vertices()
      << "\n Num local edges: " << graph.num_local_edges()
      //<< "\n Begin edge id: " << graph.global_eid(0)
      << "\n Edge balance ratio: "
      << float(graph.num_local_edges())/graph.num_edges()
      << std::endl;

  dc.cout() << "Creating engine" << std::endl;
  engine_type engine(dc, graph, exec_type, clopts);

  // Add error reporting to the engine
  const bool success = engine.add_edge_aggregator<error_aggregator>
    ("error", error_aggregator::map, error_aggregator::finalize) &&
    engine.aggregate_periodic("error", interval);
  ASSERT_TRUE(success);


  // Signal all vertices on the vertices on the left (liberals)
  engine.map_reduce_vertices<graphlab::empty>(als_vertex_program::signal_left);
  info = graph.map_reduce_edges<stats_info>(count_edges);
  dc.cout()<<"Training edges: " << info.training_edges << " validation edges: " << info.validation_edges << std::endl;

  // Run ALS ---------------------------------------------------------
  dc.cout() << "Running ALS" << std::endl;
  timer.start();
  engine.start();

  const double runtime = timer.current_time();
  dc.cout() << "----------------------------------------------------------"
            << std::endl
            << "Final Runtime (seconds):   " << runtime
            << std::endl
            << "Updates executed: " << engine.num_updates() << std::endl
            << "Update Rate (updates/second): "
            << engine.num_updates() / runtime << std::endl;

  // Compute the final training error -----------------------------------------
  dc.cout() << "Final error: " << std::endl;
  engine.aggregate_now("error");

  // Make predictions ---------------------------------------------------------
  if(!predictions.empty()) {
    std::cout << "Saving predictions" << std::endl;
    const bool gzip_output = false;
    const bool save_vertices = false;
    const bool save_edges = true;
    const size_t threads_per_machine = 2;

    //save the predictions
    graph.save(predictions, prediction_saver(),
               gzip_output, save_vertices,
               save_edges, threads_per_machine);
    //save the linear model
    graph.save(predictions + ".U", linear_model_saver_U(),
		gzip_output, save_edges, save_vertices, threads_per_machine);
    graph.save(predictions + ".V", linear_model_saver_V(),
		gzip_output, save_edges, save_vertices, threads_per_machine);

  }

  graphlab::vertex_set search_root;
  graphlab::vertex_set neighbors1;
  graphlab::vertex_set neighbors2;
  while(1) {
    int uid;
    if (dc.procid() == 0) {
      std::cout << "Enter User ID (-1) to quit: " ;
      std::cin >> uid;
    }
    dc.broadcast(uid, dc.procid() == 0);
    if (uid == -1) break;
    // does someone own uid?
    int tuid = graph.contains_vertex(uid);
    dc.all_reduce(tuid);
    if (tuid == 0) {
      if (dc.procid() == 0) {
        std::cout << "User " << uid << " does not exist\n";
      }
      continue;
    }
    // every search for user ID uid.
    map_join all_training;
    if (graph.contains_vertex(uid)) {
      graph_type::vertex_type vtx(graph.vertex(uid));
      graph_type::local_vertex_type lvtx(vtx);
      foreach(graph_type::local_edge_type edge, lvtx.out_edges()) {
        graph_type::local_vertex_type target = edge.target();
        graph_type::vertex_id_type gid = target.global_id();
        all_training.data[gid] = edge.data().obs;
      }
    }
    dc.all_reduce(all_training);
    // print the training data
    if (dc.procid() == 0) {
      std::cout << "Top 10 rated movies:\n";
      std::vector<std::pair<double, graphlab::vertex_id_type> > top10 = all_training.get_top_k(10) ;
      for(size_t i = 0;i < top10.size(); ++i) {
        graphlab::vertex_id_type gid = top10[i].second;
        int printingid = - gid - SAFE_NEG_OFFSET;
        std::cout << "\t" << printingid;
        if (mlist.find(gid) != mlist.end()) {
          std::cout << ": " << mlist[gid];
        }
        std::cout << " = " << top10[i].first << "\n";
      }
    }

    search_root = graph.empty_set();
    map_join all_predict;
    // now for the recommendations.
    bool is_master = graph.contains_vertex(uid) &&
        graph_type::local_vertex_type(graph.vertex(uid)).owned();
    // broadcast the user vector
    vec_type factor;

    search_root.make_explicit(graph);
    if (is_master) {
      factor = graph.vertex(uid).data().factor;
      graph_type::local_vertex_type lvtx(graph.vertex(uid));
      search_root.set_lvid(lvtx.id());
    }

    graph.sync_vertex_set_master_to_mirrors(search_root);
    dc.broadcast(factor, is_master);

    neighbors1 = graph.empty_set();
    neighbors2 = graph.empty_set();

    neighbors1 = graph.neighbors(search_root, graphlab::OUT_EDGES);
    neighbors2 = graph.neighbors(search_root, graphlab::IN_EDGES);
    neighbors1 = graph.neighbors(search_root, graphlab::OUT_EDGES);

    // now loop through all the vertices.
    for (size_t i = 0;i < graph.num_local_vertices(); ++i) {
      graph_type::local_vertex_type lvtx(graph.l_vertex(i));
      if (neighbors1.l_contains(i) && lvtx.owned() && (int)(lvtx.global_id()) < 0) {
        double pred = lvtx.data().factor.dot(factor);
        all_predict.data[lvtx.global_id()] = pred;
      }
    }
    dc.all_reduce(all_predict);
    if (dc.procid() == 0) {
      std::cout << "Top 10 predicted movies:\n";
      std::vector<std::pair<double, graphlab::vertex_id_type> > top10 = all_predict.get_top_k(10) ;
      for(size_t i = 0;i < top10.size(); ++i) {
        graphlab::vertex_id_type gid = top10[i].second;
        int printingid = - gid - SAFE_NEG_OFFSET;
        std::cout << "\t" << printingid;
        if (mlist.find(gid) != mlist.end()) {
          std::cout << ": " << mlist[gid];
        }
        std::cout << " = " << top10[i].first << "\n";
      }
    }
  }



  graphlab::mpi_tools::finalize();
  return EXIT_SUCCESS;
} // end of main



