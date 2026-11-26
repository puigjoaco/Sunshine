/** @file tests/integration/linux/kwin_pointer_qt.cc
 * @brief Observe modifier ownership on pointer events in the private KWin harness.
 */
#include <QApplication>
#include <QFile>
#include <QMouseEvent>
#include <QTextStream>
#include <QWheelEvent>
#include <QWidget>

/** @brief Fullscreen private receiver that persists each click and wheel modifier state. */
class Receiver: public QWidget {
public:
  /** @brief Create the isolated event log. @param path Harness-owned output filename. */
  explicit Receiver(const QString &path): file(path) { file.open(QIODevice::WriteOnly); }
protected:
  /** @brief Record a press. @param event Qt pointer event delivered by KWin. */
  void mousePressEvent(QMouseEvent *event) override {
    QTextStream stream(&file);
    stream << "button " << int(event->button()) << " ctrl " << event->modifiers().testFlag(Qt::ControlModifier) << Qt::endl;
    file.flush();
  }
  /** @brief Record a wheel event. @param event Qt wheel event delivered by KWin. */
  void wheelEvent(QWheelEvent *event) override {
    QTextStream stream(&file);
    stream << "wheel ctrl " << event->modifiers().testFlag(Qt::ControlModifier) << Qt::endl;
    file.flush();
  }
private:
  QFile file; ///< Harness-owned event log.
};

/** @brief Run the private pointer receiver; no input devices are opened. */
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (argc != 2) return 2;
  Receiver receiver(argv[1]);
  receiver.showFullScreen();
  return app.exec();
}
