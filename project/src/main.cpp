#include <iostream> // std::cout, std::cerr
#include <cstdlib> // EXIT_SUCCESS, EXIT_FAILURE
#include <vector> // std::vector<>
#include <algorithm> // std::min()
#include <cmath> // std::pow(), std::sqrt(), std::sin(), std::atan()
#include <exception> // std::exception
#include <string> // std::string
#include <functional> // std::function<>, std::bind(), std::placeholders::_1
#include <limits> // std::numeric_limits<>
#include <map> // std::map<>

#include <opencv2/imgproc/imgproc.hpp> // cv::cvtColor(), CV_BGR2RGB cv::threshold(),
                                       // cv::findContours(), cv::drawContours(),
                                       // cv::THRESH_BINARY, CV_RETR_TREE, CV_CHAIN_APPROX_SIMPLE,
                                       // cv::BORDER_REPLICATE, cv::filter2D()
#include <opencv2/highgui/highgui.hpp> // cv::imread(), CV_LOAD_IMAGE_COLOR, cv::WINDOW_NORMAL,
                                       // cv::imshow(), cv::waitKey(), cv::namedWindow()

                                       // cv::Mat, cv::Scalar, cv::Vec4i, cv::Point, cv::norm(),
                                       // cv::NORM_L2, CV_64FC1, CV_64FC1, cv::Mat_<>

#include <boost/math/special_functions/sign.hpp> // boost::math::sign()
#include <boost/algorithm/string/predicate.hpp> // boost::iequals()

#if defined(__gnu_linux__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <boost/math/constants/constants.hpp> // boost::math::constants::pi<>()
#include <boost/program_options/options_description.hpp> // boost::program_options::options_description,
                                                         // boost::program_options::value<>
#include <boost/program_options/variables_map.hpp> // boost::program_options::variables_map,
                                                   // boost::program_options::store(),
                                                   // boost::program_options::notify()
#include <boost/program_options/parsers.hpp> // boost::program_options::cmd_line::parser
#include <boost/filesystem/operations.hpp> // boost::filesystem::exists()
#include <boost/filesystem/convenience.hpp> // boost::filesystem::change_extension()

#if defined(__gnu_linux__)
#pragma GCC diagnostic pop
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(_WIN32)
#include <windows.h> // CONSOLE_SCREEN_BUFFER_INFO, GetConsoleScreenBufferInfo, GetStdHandle, STD_OUTPUT_HANDLE
#elif defined(__unix__)
#include <sys/ioctl.h> // struct winsize, ioctl(), TIOCGWINSZ
#endif

/**
 * @file
 * @todo Add option to specify the initial contour from command line
 * @todo See what Sobel kernel does with the image (or come up with a better kernel)
 * @todo figure out how to parallelize convolution (it has to be possible if the input
 *       and output images are different)
 * @todo pixel selection, dump it
 * @todo write a cute readme
 * @todo print progress (e.g. average variances, L2 norm of differential level set etc)
 * @todo line color from command line
 * @todo add color list
 * @todo add capability to set the contour by clicking twice on the image (rectangle/circle)
 * @todo oh, and reinitialization; add interval parameter
 */

typedef unsigned char uchar;
typedef unsigned long ulong;
typedef std::vector<std::vector<double>> levelset;

/**
 * @brief The Region enum
 * @sa region_variance
 */
enum Region { Inside, Outside };

/**
 * @brief Enum for specifying overlay text in the image
 * @sa overlay_color
 */
enum TextPosition { TopLeft, TopRight, BottomLeft, BottomRight };

/**
 * @brief Struct for holding basic parameters of a font
 */
struct FontParameters
{
  FontParameters(int font_face,
                 double font_scale,
                 int font_thickness,
                 int font_linetype,
                 int baseline)
    : face(font_face)
    , scale(font_scale)
    , thickness(font_thickness)
    , type(font_linetype)
    , baseline(baseline)
  {}
  const int face;
  const double scale;
  const int thickness;
  const int type;
  int baseline;
};

/**
 * @brief Class for calculating function values on the level set in parallel.
 *        Specifically, meant for taking regularized delta function on the level set.
 *
 * Credit to maythe4thbewithu (http://goo.gl/jPtLI2) for the idea.
 */
class ParallelPixelFunction : public cv::ParallelLoopBody
{
public:
  /**
   * @brief Constructor
   * @param _data  Level set
   * @param _w     Width of the level set matrix
   * @param _delta Any function
   */
  ParallelPixelFunction(cv::Mat & _data,
                        int _w,
                        std::function<double(double)> _func)
    : data(_data)
    , w(_w)
    , func(_func)
  {}
  /**
   * @brief Needed by cv::parallel_for_
   * @param r Range of all indices (as if the level set is flatten)
   */
  virtual void operator () (const cv::Range & r) const
  {
    for(int i = r.start; i != r.end; ++i)
      data.at<double>(i / w, i % w) = func(data.at<double>(i / w, i % w));
  }

private:
  cv::Mat & data;
  const int w;
  const std::function<double(double)> func;
};

/**
 * @brief Calculates the terminal/console width.
 *        Should work on all popular platforms.
 * @return Terminal width
 * @note Untested on Windows and MacOS.
 *       Credit to user 'quantum': http://stackoverflow.com/q/6812224/4056193
 */
int
get_terminal_width()
{
#if defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
  return static_cast<int>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
#elif defined(__unix__)
  struct winsize max;
  ioctl(0, TIOCGWINSZ, &max);
  return static_cast<int>(max.ws_col);
#endif
}

/**
 * @brief Regularized (smoothed) Heaviside step function
 * @f[ $H_\epsilon(x)=\frac{1}{2}\Big(1+\frac{2}{\pi}\atan\frac{x}{\epsilon}\Big)$ @f]
 * @param x   Argument of the step function
 * @param eps Smoothing parameter
 * @return Value of the step function at x
 */
constexpr double
regularized_heaviside(double x,
                      double eps = 1)
{
  const double pi = boost::math::constants::pi<double>();
  return (1 + 2 / pi * std::atan(x / eps)) / 2;
}

/**
 * @brief Regularized (smoothed) Dirac delta function
 * @f[ $\delta_\epsilon(x)=\frac{\epsilon}{\pi(\epsilon^2+x^2)}$ @f]
 * @param x   Argument of the delta function
 * @param eps Smoothing parameter
 * @return Value of the delta function at x
 */
constexpr double
regularized_delta(double x,
                  double eps = 1)
{
  const double pi = boost::math::constants::pi<double>();
  return eps / (pi * (std::pow(eps, 2) + std::pow(x, 2)));
}

/**
 * @brief Calculates average regional variance
 * @f[ $c_i = \frac{\int_\Omega I_i(x,y)g(u(x,y))\mathrm{d}x\mathrm{d}y}{
                    \int_\Omega g(u(x,y))\mathrm{d}x\mathrm{d}y}$ @f],
 * where @f[ $u(x,y)$ @f] is the level set function,
 * @f[ $I_i$ @f] is the @f[ $i$ @f]-th channel in the image and
 * @f[ $g$ @f] is either the Heaviside function @f[$H(x)$@f]
 * (for region encolosed by the contour) or @f[$1-H(x)$@f] (for region outside
 * the contour).
 * @param img       Input image (channel), @f[ $I_i(x,y)$ @f]
 * @param u         Level set, @f[ $u(x,y)$ @f]
 * @param h         Height of the image
 * @param w         Width of the image
 * @param region    Region either inside or outside the contour
 * @param heaviside Heaviside function, @f[ $H(x)$ @f]
 *                  One might also try different regularized heaviside functions
 *                  or even a non-smoothed one; that's why we've left it as a parameter
 * @return          Average variance of the given region in the image
 * @sa variance_penalty
 */
double
region_variance(const cv::Mat & img,
                const cv::Mat & u,
                int h,
                int w,
                Region region,
                std::function<double(double)> heaviside)
{
  double nom = 0.0,
         denom = 0.0;
  auto H = (region == Region::Inside)
             ? heaviside
             : [&heaviside](double x) -> double { return 1 - heaviside(x); };
  for(int i = 0; i < h; ++i)
    for(int j = 0; j < w; ++j)
    {
      double h = H(u.at<double>(i, j));
      nom += img.at<uchar>(i, j) * h;
      denom += h;
    }
  return nom / denom;
}

/**
 * @brief Creates a level set with rectangular zero level set
 * @param w Width of the level set matrix
 * @param h Height of the level set matrix
 * @param l Offset in pixels from the underlying image borders
 * @return The levelset
 * @todo Add support for offsets from all borders
 */
cv::Mat
levelset_rect(int h,
              int w,
              int l)
{
  cv::Mat u(h, w, CV_64FC1);
  u.setTo(cv::Scalar(1));
  for(int i = 0; i < l; ++i)
  {
    u.row(i) = cv::Scalar(-1);
    u.row(h - i - 1) = cv::Scalar(-1);
  }
  for(int j = 0; j < l; ++j)
  {
    u.col(j) = cv::Scalar(-1);
    u.col(w - j - 1) = cv::Scalar(-1);
  }

  return u;
}

/**
 * @brief Creates a level set with circular zero level set
 * @param w Width of the level set matrix
 * @param h Height of the level set matrix
 * @param d Diameter of the circle in relative units
 *          Its value must be within (0, 1); 1 indicates that
 *          the diameter is minimum of the image dimensions
 * @return The level set
 */
cv::Mat
levelset_circ(int h,
              int w,
              double d)
{
  cv::Mat u(h, w, CV_64FC1);

  const int r = std::min(w, h) * d / 2;
  const int mid_x = w / 2;
  const int mid_y = h / 2;

  for(int i = 0; i < h; ++i)
    for(int j = 0; j < w; ++j)
    {
      const double d = std::sqrt(std::pow(mid_x - i, 2) +
                                 std::pow(mid_y - j, 2));
      if(d < r) u.at<double>(i, j) = 1;
      else      u.at<double>(i, j) = -1;
    }

  return u;
}

/**
 * @brief Creates a level set with a checkerboard pattern at zero level
 *        The zero level set is found via the formula
 *        @f[ $ \mathrm{sign}\sin\Big(\frac{x}{5}\Big)\sin\Big(\frac{y}{5}\Big) $ @f].
 * @param w Width of the level set matrix
 * @param h Height of the level set matrix
 * @return The levelset
 */
cv::Mat
levelset_checkerboard(int h,
                      int w)
{
  cv::Mat u(h, w, CV_64FC1);
  const double pi = boost::math::constants::pi<double>();
  for(int i = 0; i < h; ++i)
    for(int j = 0; j < w; ++j)
      u.at<double>(i, j) = (boost::math::sign(std::sin(pi * i / 5) *
                                              std::sin(pi * j / 5)));
  return u;
}

/**
 * @brief Creates a contour from the level set.
 *        In the contour matrix, the negative values are replaced by 0,
 *        whereas the positive values are replaced by 255.
 *        This convention is kept in mind later on.
 * @param u Level set
 * @return Contour
 * @sa draw_contour
 */
cv::Mat
levelset2contour(const cv::Mat & u)
{
  const int h = u.rows;
  const int w = u.cols;
  cv::Mat c(h, w, CV_8UC1);

  for(int i = 0; i < h; ++i)
    for(int j = 0; j < w; ++j)
      c.at<uchar>(i, j) = u.at<double>(i, j) <= 0 ? 0 : 255;

  return c;
}

/**
 * @brief Draws the zero level set on a given image
 * @param dst The image where the contour is placed.
 * @param u   The level set, the zero level of which is plotted.
 * @return 0
 * @sa levelset2contour
 */
int
draw_contour(cv::Mat & dst,
             const cv::Mat & u)
{
  cv::Mat th;
  std::vector<std::vector<cv::Point>> cs;
  std::vector<cv::Vec4i> hier;

  cv::Mat c = levelset2contour(u);
  cv::threshold(c, th, 100, 255, cv::THRESH_BINARY);
  cv::findContours(th, cs, hier, CV_RETR_TREE, CV_CHAIN_APPROX_SIMPLE);

  int idx = 0;
  for(; idx >= 0; idx = hier[idx][0])
  {
    cv::Scalar color(255, 0, 0); // blue
    cv::drawContours(dst, cs, idx, color, 1, 8, hier);
  }

  return 0;
}

/**
 * @brief Calculates variance penalty matrix,
 * @f[ $\lambda_i\int_\Omega|I_i(x,y)-c_i|^2 g(u(x,y))\mathrm{d}x\mathrm{d}y$ @f],
 * where @f[ $u(x,y)$ @f] is the level set function,
 * @f[ $I_i$ @f] is the @f[ $i$ @f]-th channel in the image and
 * @f[ $g$ @f] is either the Heaviside function @f[$H(x)$@f]
 * (for region encolosed by the contour) or @f[$1-H(x)$@f] (for region outside
 * the contour).
 * @param channel Channel of the input image, @f[ $I_i(x,y)$ @f]
 * @param h       Height of the image
 * @param w       Width of the image
 * @param c       Variance of particular region in the image, @f[ $c_i$ @f]
 * @param lambda  Penalty parameter, @f[ $\lambda_i$ @f]
 * @return Variance penalty matrix
 * @sa region_variance
 */
cv::Mat
variance_penalty(const cv::Mat & channel,
                 int h,
                 int w,
                 double c,
                 double lambda)
{
  cv::Mat channel_term(cv::Mat::zeros(h, w, CV_64FC1));
  channel.convertTo(channel_term, channel_term.type());
  channel_term -= c;
  cv::pow(channel_term, 2, channel_term);
  channel_term *= lambda;
  return channel_term;
}

/**
 * @brief Calculates the curvature (divergence of normalized gradient)
 *        of the level set
 * @param u       The level set
 * @param h       Height of the level set matrix
 * @param w       Width of the level set matrix
 * @param kernels Kernels for forward, backward and central differences
 *                in x and y direction
 * @return Curvature
 * @todo Add LaTeX-fied formula of the discretized Laplacian
 */
cv::Mat
curvature(const cv::Mat & u,
          int h,
          int w,
          const std::map<std::string, cv::Mat> & kernels)
{
  const double eta = 1E-8;
  const double eta2 = std::pow(eta, 2);
  cv::Mat upx (h, w, CV_64FC1), upy (h, w, CV_64FC1),
          ucx2(h, w, CV_64FC1), ucy2(h, w, CV_64FC1),
          upx2(h, w, CV_64FC1), upy2(h, w, CV_64FC1),
          nx  (h, w, CV_64FC1), ny  (h, w, CV_64FC1);
  cv::filter2D(u, upx,  CV_64FC1, kernels.at("fwd_x"), cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  cv::filter2D(u, upy,  CV_64FC1, kernels.at("fwd_y"), cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  cv::filter2D(u, ucx2, CV_64FC1, kernels.at("ctr_x"), cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  cv::filter2D(u, ucy2, CV_64FC1, kernels.at("ctr_y"), cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  cv::pow(ucx2, 2, ucx2);
  cv::pow(ucy2, 2, ucy2);
  cv::pow(upx,  2, upx2);
  cv::pow(upy,  2, upy2);
  cv::sqrt(upx2 + ucy2 + eta2, nx);
  cv::sqrt(ucx2 + upy2 + eta2, ny);
  cv::divide(upx, nx, upx);
  cv::divide(upy, ny, upy);
  cv::filter2D(upx, upx, CV_64FC1, kernels.at("bwd_x"), cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  cv::filter2D(upy, upy, CV_64FC1, kernels.at("bwd_y"), cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  upx += upy;
  return upx;
}

/**
 * @brief Finds proper font color for overlay text
 *        The color is determined by the average intensity of the ROI where
 *        the text is placed
 * @param img    Image where the text is placed
 * @param txt    The text itself
 * @param pos    Position in the image (for possibilities: top left corner,
 *               top right, bottom left or bottom right)
 * @param fparam Font parameters that help to determine the dimensions of ROI
 * @param color  Reference to the color variable
 * @param p      Reference to the bottom left point of the text area
 * @return Black color, if the background is white enough; otherwise white color
 * @todo add some check if the text width/height exceeds image dimensions
 * @sa TextPosition, FontParameters
 */
int
overlay_color(const cv::Mat & img,
              const std::string txt,
              TextPosition pos,
              FontParameters & fparam,
              cv::Scalar & color,
              cv::Point & p)
{
  const cv::Scalar white = CV_RGB(255, 255, 255);
  const cv::Scalar black = CV_RGB(  0,   0,   0);
  const int threshold = 105; // bias towards black font

  const cv::Size txt_sz = cv::getTextSize(txt, fparam.face, fparam.scale, fparam.thickness, &fparam.baseline);
  const int padding = 5;
  cv::Point q;

  if(pos == TextPosition::TopLeft)
  {
    p = cv::Point(padding, padding + txt_sz.height);
    q = cv::Point(padding, padding);
  }
  else if(pos == TextPosition::TopRight)
  {
    p = cv::Point(img.cols - padding - txt_sz.width, padding + txt_sz.height);
    q = cv::Point(img.cols - padding - txt_sz.width, padding);
  }
  else if(pos == TextPosition::BottomLeft)
  {
    p = cv::Point(padding, img.rows - padding);
    q = cv::Point(padding, img.rows - padding - txt_sz.height);
  }
  else if(pos == TextPosition::BottomRight)
  {
    p = cv::Point(img.cols - padding - txt_sz.width, img.rows - padding);
    q = cv::Point(img.cols - padding - txt_sz.width, img.rows - padding - txt_sz.height);
  }

  cv::Scalar avgs = cv::mean(img(cv::Rect(q, txt_sz)));
  const double intensity_avg = 0.114*avgs[0] + 0.587*avgs[1] + 0.299*avgs[2];
  color = 255 - intensity_avg < threshold ? black : white;
  return 0;
}

int
main(int argc,
     char ** argv)
{
  std::string input_filename;
  double mu, nu, eps, tol, dt, fps;
  int max_steps;
  std::vector<double> lambda1, lambda2;
  std::string text_position;
  TextPosition pos = TextPosition::TopLeft;
  bool grayscale = false,
       write_video = false,
       overlay_text = false,
       verbose = false;

///-- Parse command line arguments
///   Negative values in multitoken are not an issue, b/c it doesn't make much sense
///   to use negative values for lambda1 and lambda2
  try
  {
    namespace po = boost::program_options;
    po::options_description desc("Allowed options", get_terminal_width());
    desc.add_options()
      ("help,h",                                                                        "this message")
      ("input,i",        po::value<std::string>(&input_filename),                       "input image")
      ("mu",             po::value<double>(&mu) -> default_value(0.5),                  "length penalty parameter")
      ("nu",             po::value<double>(&nu) -> default_value(0),                    "area penalty parameter")
      ("dt",             po::value<double>(&dt) -> default_value(1),                    "timestep")
      ("lambda1",        po::value<std::vector<double>>(&lambda1) -> multitoken(),      "penalty of variance inside the contour (default: 1's)")
      ("lambda2",        po::value<std::vector<double>>(&lambda2) -> multitoken(),      "penalty of variance outside the contour (default: 1's)")
      ("epsilon,e",      po::value<double>(&eps) -> default_value(1),                   "smoothing parameter in Heaviside/delta")
      ("tolerance,t",    po::value<double>(&tol) -> default_value(0.001),               "tolerance in stopping condition")
      ("max-steps,N",    po::value<int>(&max_steps) -> default_value(-1),               "maximum nof iterations (negative means unlimited)")
      ("fps,f",          po::value<double>(&fps) -> default_value(10),                  "video fps")
      ("overlay-pos,P",  po::value<std::string>(&text_position) -> default_value("TL"), "overlay tex position; allowed only: TL, BL, TR, BR")
      ("verbose,v",      po::bool_switch(&verbose),                                     "verbose mode")
      ("grayscale,g",    po::bool_switch(&grayscale),                                   "read in as grayscale")
      ("video,V",        po::bool_switch(&write_video),                                 "enable video output (changes the extension to .avi)")
      ("overlay-text,O", po::bool_switch(&overlay_text),                                "add overlay text")
    ;
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
    po::notify(vm);

    if(vm.count("help"))
    {
      std::cout << desc << "\n";
      return EXIT_SUCCESS;
    }
    if(! vm.count("input"))
    {
      std::cerr << "\nError: you have to specify input file name!\n\n";
      return EXIT_FAILURE;
    }
    else if(vm.count("input") && ! boost::filesystem::exists(input_filename))
    {
      std::cerr << "\nError: file \"" << input_filename << "\" does not exists!\n\n";
      return EXIT_FAILURE;
    }
    if(vm.count("dt") && dt < 0)
    {
      std::cerr << "\nCannot have negative timestep: " << dt << ".\n\n";
      return EXIT_FAILURE;
    }
    if(vm.count("lambda1"))
    {
      if(grayscale && lambda1.size() != 1)
      {
        std::cerr << "\nToo many lambda1 values for a grayscale image.\n\n";
        return EXIT_FAILURE;
      }
      else if(!grayscale && lambda1.size() != 3)
      {
        std::cerr << "\nNumber of lambda1 values must be 3 for a colored input image.\n\n";
        return EXIT_FAILURE;
      }
      if(grayscale && lambda1.size() == 1 && lambda1[0] < 0)
      {
        std::cerr << "\nThe value of lambda1 cannot be negative.\n\n";
        return EXIT_FAILURE;
      }
    }
    else if(! vm.count("lambda1"))
    {
      if(grayscale) lambda1 = {1};
      else          lambda1 = {1, 1, 1};
    }
    if(vm.count("lambda2"))
    {
      if(grayscale && lambda2.size() != 1)
      {
        std::cerr << "\nToo many lambda2 values for a grayscale image.\n\n";
        return EXIT_FAILURE;
      }
      else if(!grayscale && lambda2.size() != 3)
      {
        std::cerr << "\nNumber of lambda2 values must be 3 for a colored input image.\n\n";
        return EXIT_FAILURE;
      }
      if(grayscale && lambda2.size() == 1 && lambda2[0] < 0)
      {
        std::cerr << "\nThe value of lambda1 cannot be negative.\n\n";
        return EXIT_FAILURE;
      }
    }
    else if(! vm.count("lambda2"))
    {
      if(grayscale) lambda2 = {1};
      else          lambda2 = {1, 1, 1};
    }
    if(vm.count("eps") && eps < 0)
    {
      std::cerr << "\nCannot have negative smoothing parameter: " << eps << ".\n\n";
      return EXIT_FAILURE;
    }
    if(vm.count("tol") && tol < 0)
    {
      std::cerr << "\nCannot have negative tolerance: " << tol << ".\n\n";
      return EXIT_FAILURE;
    }
    if(vm.count("overlay-pos"))
    {
      if     (boost::iequals(text_position, "TL")) pos = TextPosition::TopLeft;
      else if(boost::iequals(text_position, "BL")) pos = TextPosition::BottomLeft;
      else if(boost::iequals(text_position, "TR")) pos = TextPosition::TopRight;
      else if(boost::iequals(text_position, "BR")) pos = TextPosition::BottomRight;
      else
      {
        std::cerr << "\nInvalid text position requested.\n"
                  << "Correct values are: TL -- top left\n"
                  << "                    BL -- bottom left\n"
                  << "                    TR -- top right\n"
                  << "                    BR -- bottom right\n\n";
        return EXIT_FAILURE;
      }
    }
  }
  catch(std::exception & e)
  {
    std::cerr << "error: " << e.what() << "n";
    return EXIT_FAILURE;
  }

///-- Read the image (grayscale or BGR? RGB? BGR? help)
  cv::Mat _img;
  if(grayscale) _img = cv::imread(input_filename, CV_LOAD_IMAGE_GRAYSCALE);
  else          _img = cv::imread(input_filename, CV_LOAD_IMAGE_COLOR);
  if(! _img.data)
  {
    std::cerr << "\nError on opening " << input_filename << " (probably not an image)!\n\n";
    return EXIT_FAILURE;
  }

///-- Second conversion needed since we want to display a colored contour
///   on a grayscale image
  cv::Mat img;
  if(grayscale) cv::cvtColor(_img, img, CV_GRAY2RGB);
  else          img = _img;
  _img.release();

///-- Determine the constants and define functionals
  max_steps = max_steps < 0 ? std::numeric_limits<int>::max() : max_steps;
  const int h = img.rows;
  const int w = img.cols;
  const int nof_channels = grayscale ? 1 : img.channels();
  const auto heaviside = std::bind(regularized_heaviside, std::placeholders::_1, eps);
  const auto delta = std::bind(regularized_delta, std::placeholders::_1, eps);

///-- Set up overlay font
  FontParameters fparam(CV_FONT_HERSHEY_PLAIN, 0.8, 1, 0, CV_AA);

///-- Set up the video writer
  cv::VideoWriter vw;
  if(write_video)
  {
    const std::string video_filename = boost::filesystem::change_extension(input_filename, "avi").string();
    vw = cv::VideoWriter(video_filename, CV_FOURCC('X','V','I','D'), fps, img.size());
  }

///-- Define kernels for forward, backward and central differences in x and y direction
  const std::map<std::string, cv::Mat> kernels = {
    { "fwd_x", (cv::Mat_<double>(3, 3) << 0,   0,0,   0,-1,  1,0,  0,0) },
    { "fwd_y", (cv::Mat_<double>(3, 3) << 0,   0,0,   0,-1,  0,0,  1,0) },
    { "bwd_x", (cv::Mat_<double>(3, 3) << 0,   0,0,  -1, 1,  0,0,  0,0) },
    { "bwd_y", (cv::Mat_<double>(3, 3) << 0,  -1,0,   0, 1,  0,0,  0,0) },
    { "ctr_x", (cv::Mat_<double>(3, 3) << 0,   0,0,-0.5, 0,0.5,0,  0,0) },
    { "ctr_y", (cv::Mat_<double>(3, 3) << 0,-0.5,0,   0, 0,  0,0,0.5,0) },
  };

///-- Construct the level set
  cv::Mat u = levelset_checkerboard(h, w);

///-- Split the channels
  std::vector<cv::Mat> channels;
  channels.reserve(nof_channels);
  cv::split(img, channels);

///-- Find intensity sum and derive the stopping condition
  cv::Mat intensity_avg = cv::Mat(h, w, CV_64FC1, cv::Scalar::all(0));
  for(int k = 0; k < nof_channels; ++k)
  {
    cv::Mat channel(h, w, intensity_avg.type());
    channels[k].convertTo(channel, channel.type());
    intensity_avg += channel;
  }
  intensity_avg /= nof_channels;
  double stop_cond = tol * cv::norm(intensity_avg, cv::NORM_L2);
  intensity_avg.release();

///-- Timestep loop
  for(int t = 0; t < max_steps; ++t)
  {
    cv::Mat u_diff(cv::Mat::zeros(h, w, CV_64FC1));

///-- Channel loop
    for(int k = 0; k < nof_channels; ++k)
    {
      cv::Mat channel = channels[k];
///-- Find the average regional variances
      const double c1 = region_variance(channel, u, h, w, Region::Inside, heaviside);
      const double c2 = region_variance(channel, u, h, w, Region::Outside, heaviside);

///-- Calculate the contribution of one channel to the level set
      const cv::Mat variance_inside = variance_penalty(channel, h, w, c1, lambda1[k]);
      const cv::Mat variance_outside = variance_penalty(channel, h, w, c2, lambda2[k]);
      u_diff += -variance_inside + variance_outside;
    }
///-- Calculate the curvature (divergence of normalized gradient)
    const cv::Mat kappa = curvature(u, h, w, kernels);

///-- Mash the terms together
    u_diff /= nof_channels;
    u_diff -= nu;
    kappa *= mu;
    u_diff += kappa;
    u_diff *= dt;

///-- Run delta function on the level set
    cv::Mat u_cp = u.clone();
    cv::parallel_for_(cv::Range(0, h * w), ParallelPixelFunction(u_cp, w, delta));

///-- Shift the level set
    cv::multiply(u_diff, u_cp, u_diff);
    u += u_diff;

///-- Save the frame
    if(write_video)
    {
      cv::Mat nw_img = img.clone();
      draw_contour(nw_img, u);
      if(overlay_text)
      {
        const std::string txt = "time = " + std::to_string(t);
        cv::Scalar color;
        cv::Point p;
        overlay_color(img, txt, pos, fparam, color, p);
        cv::putText(nw_img, txt, p, fparam.face, fparam.scale, color, fparam.thickness, fparam.type);
      }
      vw.write(nw_img);
    }

///-- Check if we have achieved the desired precision
    const double u_diff_norm = cv::norm(u_diff, cv::NORM_L2);
    if(u_diff_norm <= stop_cond) break;
  }

  return EXIT_SUCCESS;
}
