/**
 * @file tests/integration/linux/kwin_text_qt.cc
 * @brief Record Qt text and Return events from a private nested compositor.
 */
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QKeyEvent>
#include <QStringList>
#include <QTextStream>

/** @brief Count Escape presses independently so remapped Caps must never cancel a dialog. */
class Edit: public QLineEdit {
public:
  QString escapePath; ///< Private Escape counter output.
  int escapes = 0; ///< Number of received Escape presses.
  /** @brief Persist Escape count. */
  void saveEscapes() {
    QFile file(escapePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { QTextStream stream(&file); stream << escapes; }
  }
protected:
  /** @brief Count Escape then forward. @param event Delivered key press. */
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) { ++escapes; saveEscapes(); }
    QLineEdit::keyPressEvent(event);
  }
};

/** @brief Run one focused edit; write every change and count unexpected Return presses. */
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (argc < 2) {
    return 2;
  }
  Edit edit;
  edit.escapePath = QString(argv[1]) + ".escapes";
  edit.saveEscapes();
  edit.setWindowTitle("Sunshine isolated Unicode test");
  edit.resize(800, 200);
  int returns = 0;
  const bool lines = argc == 3;
  QStringList submitted;
  const auto save = [&] {
    QFile file(argv[1]);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      QTextStream stream(&file);
      stream << returns << "\n"
             << (lines ? submitted.join("\n") + (submitted.isEmpty() ? "" : "\n") + edit.text() : edit.text());
    }
  };
  QObject::connect(&edit, &QLineEdit::textChanged, save);
  QObject::connect(&edit, &QLineEdit::returnPressed, [&] {
    ++returns;
    if (lines) {
      submitted.append(edit.text());
      edit.clear();
    }
    save();
  });
  edit.show();
  edit.setFocus();
  save();
  return app.exec();
}
