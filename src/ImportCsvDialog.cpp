#include "ImportCsvDialog.h"
#include "ui_ImportCsvDialog.h"
#include "sqlitedb.h"
#include "csvparser.h"
#include "sqlite.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QDateTime>
#include <QTextCodec>
#include <QCompleter>
#include <QComboBox>
#include <QFile>
#include <QTextStream>
#include <QSettings>
#include <QDebug>
#include <QFileInfo>
#include <memory>

// Enable this line to show basic performance stats after each imported CSV file. Please keep in mind that while these
// numbers might help to estimate the performance of the algorithm, this is not a proper benchmark.
//#define CSV_BENCHMARK

#ifdef CSV_BENCHMARK
#include <QElapsedTimer>
#endif

ImportCsvDialog::ImportCsvDialog(const QStringList &filenames, DBBrowserDB* db, QWidget* parent)
    : QDialog(parent),
      ui(new Ui::ImportCsvDialog),
      csvFilenames(filenames),
      pdb(db)
{
    ui->setupUi(this);

    // Get the actual file name out of the provided path and use it as the default table name for import
    // For importing several files at once, the fields have to be the same so we can safely use the first
    QFileInfo file(filenames.first());
    ui->editName->setText(file.baseName());

    // Create a list of all available encodings and create an auto completion list from them
    QStringList encodingList;
    foreach(QString enc, QTextCodec::availableCodecs())
        encodingList.push_back(enc);
    encodingCompleter = new QCompleter(encodingList, this);
    encodingCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    ui->editCustomEncoding->setCompleter(encodingCompleter);

    // Load last used settings and apply them
    QSettings settings(QApplication::organizationName(), QApplication::organizationName());
    ui->checkboxHeader->setChecked(settings.value("importcsv/firstrowheader", false).toBool());
    ui->checkBoxTrimFields->setChecked(settings.value("importcsv/trimfields", true).toBool());
    ui->checkBoxSeparateTables->setChecked(settings.value("importcsv/separatetables", false).toBool());
    setSeparatorChar(QChar(settings.value("importcsv/separator", ',').toInt()));
    setQuoteChar(QChar(settings.value("importcsv/quotecharacter", '"').toInt()));
    setEncoding(settings.value("importcsv/encoding", "UTF-8").toString());

    // Prepare and show interface depending on how many files are selected
    if (csvFilenames.length() > 1)
    {
        ui->separateTables->setVisible(true);
        ui->checkBoxSeparateTables->setVisible(true);
        ui->filePickerBlock->setVisible(true);
        selectFiles();
    }
    else if (csvFilenames.length() == 1)
    {
        ui->separateTables->setVisible(false);
        ui->checkBoxSeparateTables->setVisible(false);
        ui->filePickerBlock->setVisible(false);
    }

    selectedFile = csvFilenames.first();
    updatePreview();
    checkInput();
}

ImportCsvDialog::~ImportCsvDialog()
{
    delete ui;
}

namespace {
void rollback(
        ImportCsvDialog* dialog,
        DBBrowserDB* pdb,
        QProgressDialog& progress,
        const QString& savepointName,
        size_t nRecord,
        const QString& message)
{
    progress.hide();
    QApplication::restoreOverrideCursor();  // restore original cursor
    if(!message.isEmpty())
    {
        QString sCSVInfo = QObject::tr("Error importing data");
        if(nRecord)
            sCSVInfo += QObject::tr(" from record number %1").arg(nRecord);
        QString error = sCSVInfo + QObject::tr(".\n%1").arg(message);
        QMessageBox::warning(dialog, QApplication::applicationName(), error);
    }
    pdb->revertToSavepoint(savepointName);
}
}

class CSVImportProgress : public CSVProgress
{
public:
    explicit CSVImportProgress(size_t filesize)
    {
        m_pProgressDlg = new QProgressDialog(
                    QObject::tr("Decoding CSV file..."),
                    QObject::tr("Cancel"),
                    0,
                    filesize);
        m_pProgressDlg->setWindowModality(Qt::ApplicationModal);
    }

    ~CSVImportProgress()
    {
        delete m_pProgressDlg;
    }

    void start()
    {
        m_pProgressDlg->show();
    }

    bool update(size_t pos)
    {
        m_pProgressDlg->setValue(pos);
        qApp->processEvents();

        return !m_pProgressDlg->wasCanceled();
    }

    void end()
    {
        m_pProgressDlg->hide();
    }

private:
    QProgressDialog* m_pProgressDlg;
};

void ImportCsvDialog::accept()
{
    // save settings
    QSettings settings(QApplication::organizationName(), QApplication::organizationName());
    settings.beginGroup("importcsv");
    settings.setValue("firstrowheader", ui->checkboxHeader->isChecked());
    settings.setValue("separator", currentSeparatorChar());
    settings.setValue("quotecharacter", currentQuoteChar());
    settings.setValue("trimfields", ui->checkBoxTrimFields->isChecked());
    settings.setValue("separatetables", ui->checkBoxSeparateTables->isChecked());
    settings.setValue("encoding", currentEncoding());
    settings.endGroup();

    // Get all the selected files and start the import
    if (ui->filePickerBlock->isVisible())
    {
        for (int i = 0; i < ui->filePicker->count(); i++) {
            auto item = ui->filePicker->item(i);
            if (item->checkState() == Qt::Checked) {
                importCsv(item->data(Qt::DisplayRole).toString(), item->data(Qt::UserRole).toString());
            }
        }
    }
    else
    {
        importCsv(csvFilenames.first());
    }

    QApplication::restoreOverrideCursor();  // restore original cursor
    QDialog::accept();
}

void ImportCsvDialog::updatePreview()
{
    // Show/hide custom quote/separator input fields
    ui->editCustomQuote->setVisible(ui->comboQuote->currentIndex() == ui->comboQuote->count()-1);
    ui->editCustomSeparator->setVisible(ui->comboSeparator->currentIndex() == ui->comboSeparator->count()-1);
    ui->editCustomEncoding->setVisible(ui->comboEncoding->currentIndex() == ui->comboEncoding->count()-1);

    // Get preview data
    QFile file(selectedFile);
    file.open(QIODevice::ReadOnly);

    CSVParser csv(ui->checkBoxTrimFields->isChecked(), currentSeparatorChar(), currentQuoteChar());

    QTextStream tstream(&file);
    tstream.setCodec(currentEncoding().toUtf8());
    csv.parse(tstream, 20);
    file.close();

    // Analyse CSV file
    sqlb::FieldVector fieldList = generateFieldList(selectedFile);

    // Reset preview widget
    ui->tablePreview->clear();
    ui->tablePreview->setColumnCount(fieldList.size());

    // Exit if there are no lines to preview at all
    if(fieldList.size() == 0)
        return;

    // Use first row as header if necessary
    CSVParser::TCSVResult::const_iterator itBegin = csv.csv().begin();
    if(ui->checkboxHeader->isChecked())
    {
        ui->tablePreview->setHorizontalHeaderLabels(*itBegin);
        ++itBegin;
    }

    // Fill data section
    ui->tablePreview->setRowCount(std::distance(itBegin, csv.csv().end()));

    for(CSVParser::TCSVResult::const_iterator ct = itBegin;
        ct != csv.csv().end();
        ++ct)
    {
        for(QStringList::const_iterator it = ct->begin(); it != ct->end(); ++it)
        {
            int rowNum = std::distance(itBegin, ct);
            if(it == ct->begin())
            {
                ui->tablePreview->setVerticalHeaderItem(
                            rowNum,
                            new QTableWidgetItem(QString::number(rowNum + 1)));
            }
            ui->tablePreview->setItem(
                        rowNum,
                        std::distance(ct->begin(), it),
                        new QTableWidgetItem(*it));
        }
    }
}

void ImportCsvDialog::checkInput()
{
    bool allowImporting = false;
    if (ui->filePickerBlock->isVisible()) {
        bool checkedItem = false;
        for (int i = 0; i < ui->filePicker->count(); i++) {
            if (ui->filePicker->item(i)->checkState() == Qt::Checked) checkedItem = true;
        }
        allowImporting = !ui->editName->text().isEmpty() && checkedItem;
    } else {
        allowImporting = !ui->editName->text().isEmpty();
    }

    if (ui->filePicker->currentItem()) {
        ui->filePicker->currentItem()->setData(Qt::UserRole, ui->editName->text());
    }

    ui->matchSimilar->setEnabled(ui->filePicker->currentItem() != nullptr);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(allowImporting);
}

void ImportCsvDialog::selectFiles()
{
    for (auto fileName : csvFilenames) {
        auto fInfo = QFileInfo(fileName);
        auto item = new QListWidgetItem();
        item->setText(fileName);
        item->setData(Qt::UserRole, fInfo.baseName());
        item->setCheckState(Qt::Checked);
        ui->filePicker->addItem(item);
    }

    connect(ui->filePicker, SIGNAL(itemClicked(QListWidgetItem*)), SLOT(updateSelectedFilePreview(QListWidgetItem*)));
}

void ImportCsvDialog::updateSelectedFilePreview(QListWidgetItem* item)
{
    selectedFile = item->data(Qt::DisplayRole).toString();
    QFileInfo fileInfo(selectedFile);
    if (ui->checkBoxSeparateTables->isChecked()) {
        ui->editName->setText(item->data(Qt::UserRole).toString());
    } else {
        ui->editName->setText(fileInfo.baseName());
    }
    updatePreview();
    checkInput();
}

void ImportCsvDialog::updateSelection(bool selected)
{
    for (int i = 0; i < ui->filePicker->count(); i++)
        ui->filePicker->item(i)->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
    ui->toggleSelected->setText(selected ? tr("Deselect All") : tr("Select All"));
    checkInput();
}

void ImportCsvDialog::matchSimilar()
{
    auto item = ui->filePicker->currentItem();
    auto selectedHeader = generateFieldList(item->data(Qt::DisplayRole).toString());

    for (int i = 0; i < ui->filePicker->count(); i++)
    {
        auto item = ui->filePicker->item(i);
        auto header = generateFieldList(item->data(Qt::DisplayRole).toString());
        bool matchingHeader = false;

        if (selectedHeader.count() == header.count())
        {
            matchingHeader = std::equal(selectedHeader.begin(), selectedHeader.end(), header.begin(),
                                        [](const sqlb::FieldPtr& item1, const sqlb::FieldPtr& item2) -> bool {
                                            return (item1->name() == item2->name());
                                        });
            if (matchingHeader) {
                item->setCheckState(Qt::Checked);
                item->setBackgroundColor(Qt::green);
            }
        }
        else
        {
            item->setCheckState(Qt::Unchecked);
            item->setBackground(Qt::white);
        }
    }

    checkInput();
}

CSVParser ImportCsvDialog::parseCSV(const QString &fileName, qint64 count)
{
    // Parse all csv data
    QFile file(fileName);
    file.open(QIODevice::ReadOnly);

    CSVParser csv(ui->checkBoxTrimFields->isChecked(), currentSeparatorChar(), currentQuoteChar());
    // If count is one, we only want the header, no need to see progress
    if (count != 1) csv.setCSVProgress(new CSVImportProgress(file.size()));

    QTextStream tstream(&file);
    tstream.setCodec(currentEncoding().toUtf8());
    csv.parse(tstream, count);
    file.close();

    return csv;
}

sqlb::FieldVector ImportCsvDialog::generateFieldList(const QString& filename)
{
    // Parse the first couple of records of the CSV file and only analyse them
    CSVParser parser = parseCSV(filename, 20);

    // If there is no data, we don't return any fields
    if(parser.csv().size() == 0)
        return sqlb::FieldVector();

    // How many columns are there in the CSV file?
    int columns = 0;
    for(int i=0;i<parser.csv().size();i++)
    {
        if(parser.csv().at(i).size() > columns)
            columns = parser.csv().at(i).size();
    }

    // Generate field names. These are either taken from the first CSV row or are generated in the format of "fieldXY" depending on the user input
    sqlb::FieldVector fieldList;
    for(int i=0;i<columns;i++)
    {
        QString fieldname;

        // Only take the names from the CSV file if the user wants that and if the first row in the CSV file has enough columns
        if(ui->checkboxHeader->isChecked() && i < parser.csv().at(0).size())
        {
            // Take field name from CSV and remove invalid characters
            fieldname = parser.csv().at(0).at(i);
            fieldname.replace("`", "");
            fieldname.replace(" ", "");
            fieldname.replace('"', "");
            fieldname.replace("'","");
            fieldname.replace(",","");
            fieldname.replace(";","");
        }

        // If we don't have a field name by now, generate one
        if(fieldname.isEmpty())
            fieldname = QString("field%1").arg(i+1);

        // TODO Here's also the place to do some sort of data type analysation of the CSV data

        // Add field to the column list
        fieldList.push_back(sqlb::FieldPtr(new sqlb::Field(fieldname, "")));
    }

    return fieldList;
}

void ImportCsvDialog::importCsv(const QString& fileName, const QString &name)
{
#ifdef CSV_BENCHMARK
    // If benchmark mode is enabled start measuring the performance now
    QElapsedTimer timer;
    timer.start();
#endif

    QString tableName;

    if (csvFilenames.size() > 1 && ui->checkBoxSeparateTables->isChecked()) {
        if (name.isEmpty()) {
            QFileInfo fileInfo(fileName);
            tableName = fileInfo.baseName();
        } else {
            tableName = name;
        }
    } else {
        tableName = ui->editName->text();
    }

    // Analyse CSV file
    sqlb::FieldVector fieldList = generateFieldList(fileName);

    // Parse entire file
    CSVParser csv = parseCSV(fileName);
    if (csv.csv().size() == 0)  return;

#ifdef CSV_BENCHMARK
    qint64 timer_after_parsing = timer.elapsed();
#endif

    // Show progress dialog
    QProgressDialog progress(tr("Inserting data..."), tr("Cancel"), 0, csv.csv().size());
    progress.setWindowModality(Qt::ApplicationModal);
    progress.show();

    // Are we importing into an existing table?
    bool importToExistingTable = false;
    const sqlb::ObjectPtr obj = pdb->getObjectByName(sqlb::ObjectIdentifier("main", tableName));
    if(obj && obj->type() == sqlb::Object::Types::Table)
    {
        if(obj.dynamicCast<sqlb::Table>()->fields().size() != fieldList.size())
        {
            QMessageBox::warning(this, QApplication::applicationName(),
                                 tr("There is already a table of that name and an import into an existing table is only possible if the number of columns match."));
            return;
        } else {
            // If we are importing multiple files, we can skip the warning and perform the inserts
            // To omit further warnings there is a filter button to select all conforming sql
            if (!ui->checkBoxSeparateTables->isChecked())
                importToExistingTable = true;

            if(QMessageBox::question(this, QApplication::applicationName(), tr("There is already a table of that name. Do you want to import the data into it?"), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
                importToExistingTable = true;
            else
                return;
        }
    }

#ifdef CSV_BENCHMARK
    qint64 timer_before_insert = timer.elapsed();
#endif

    // Create a savepoint, so we can rollback in case of any errors during importing
    // db needs to be saved or an error will occur
    QString restorepointName = pdb->generateSavepointName("csvimport");
    if(!pdb->setSavepoint(restorepointName))
        return rollback(this, pdb, progress, restorepointName, 0, tr("Creating restore point failed: %1").arg(pdb->lastError()));

    // Create table
    QStringList nullValues;
    if(!importToExistingTable)
    {
        if(!pdb->createTable(sqlb::ObjectIdentifier("main", tableName), fieldList))
            return rollback(this, pdb, progress, restorepointName, 0, tr("Creating the table failed: %1").arg(pdb->lastError()));
    } else {
        // Importing into an existing table. So find out something about it's structure.

        // Prepare the values for each table column that are to be inserted if the field in the CSV file is empty. Depending on the data type
        // and the constraints of a field, we need to handle this case differently.
        sqlb::TablePtr tbl = pdb->getObjectByName(sqlb::ObjectIdentifier("main", tableName)).dynamicCast<sqlb::Table>();
        if(tbl)
        {
            foreach(const sqlb::FieldPtr& f, tbl->fields())
            {
                if(f->isInteger() && f->notnull())              // If this is an integer column but NULL isn't allowed, insert 0
                    nullValues << "0";
                else if(f->isInteger() && !f->notnull())        // If this is an integer column and NULL is allowed, insert NULL
                    nullValues << QString();
                else                                            // Otherwise (i.e. if this isn't an integer column), insert an empty string
                    nullValues << "";
            }
        }
    }

    // Prepare the INSERT statement. The prepared statement can then be reused for each row to insert
    QString sQuery = QString("INSERT INTO %1 VALUES(").arg(sqlb::escapeIdentifier(tableName));
    for(int i=1;i<=fieldList.size();i++)
        sQuery.append(QString("?%1,").arg(i));
    sQuery.chop(1); // Remove last comma
    sQuery.append(")");
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(pdb->_db, sQuery.toUtf8(), sQuery.toUtf8().length(), &stmt, nullptr);

    // now lets import all data, one row at a time
    CSVParser::TCSVResult::const_iterator itBegin = csv.csv().begin();
    if(ui->checkboxHeader->isChecked())     // If the first row contains the field names we should skip it here because this is the data import
        ++itBegin;
    for(CSVParser::TCSVResult::const_iterator it = itBegin;
        it != csv.csv().end();
        ++it)
    {
        // Bind all values
        unsigned int bound_fields = 0;
        for(int i=0;i<it->size();i++,bound_fields++)
        {
            // Empty values need special treatment, but only when importing into an existing table where we could find out something about
            // its table definition
            if(importToExistingTable && it->at(i).isEmpty() && nullValues.size() > i)
            {
                // This is an empty value. We'll need to look up how to handle it depending on the field to be inserted into.
                QString val = nullValues.at(i);
                if(!val.isNull())       // No need to bind NULL values here as that is the default bound value in SQLite
                    sqlite3_bind_text(stmt, i+1, val.toUtf8(), val.toUtf8().size(), SQLITE_TRANSIENT);
            } else {
                // This is a non-empty value. Just add it to the statement
                sqlite3_bind_text(stmt, i+1, static_cast<const char*>(it->at(i).toUtf8()), it->at(i).toUtf8().size(), SQLITE_TRANSIENT);
            }
        }

        // Insert row
        if(sqlite3_step(stmt) != SQLITE_DONE)
        {
            sqlite3_finalize(stmt);
            return rollback(this, pdb, progress, restorepointName, std::distance(itBegin, it) + 1, tr("Inserting row failed: %1").arg(pdb->lastError()));
        }

        // Reset statement for next use. Also reset all bindings to NULL. This is important, so we don't need to bind missing columns or empty values in NULL
        // columns manually.
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        // Update progress bar and check if cancel button was clicked
        unsigned int prog = std::distance(csv.csv().begin(), it);
        if(prog % 100 == 0)
            progress.setValue(prog);
        if(progress.wasCanceled())
        {
            sqlite3_finalize(stmt);
            return rollback(this, pdb, progress, restorepointName, std::distance(itBegin, it) + 1, "");
        }
    }

    // Clean up prepared statement
    sqlite3_finalize(stmt);

#ifdef CSV_BENCHMARK
    // If benchmark mode is enabled calculate the results now
    qint64 timer_after_insert = timer.elapsed();

    QMessageBox::information(this, qApp->applicationName(),
                             tr("Importing the file '%1' took %2ms. The parser took %3ms and the insertion took %4ms.")
                             .arg(fileName)
                             .arg(timer_after_insert)
                             .arg(timer_after_parsing)
                             .arg(timer_after_insert-timer_before_insert));
#endif
}

void ImportCsvDialog::setQuoteChar(const QChar& c)
{
    QComboBox* combo = ui->comboQuote;
    int index = combo->findText(c);
    if(index == -1)
    {
        combo->setCurrentIndex(combo->count());
        ui->editCustomQuote->setText(c);
    }
    else
    {
        combo->setCurrentIndex(index);
    }
}

char ImportCsvDialog::currentQuoteChar() const
{
    // The last item in the combobox is the 'Other' item; if it is selected return the text of the line edit field instead
    if(ui->comboQuote->currentIndex() == ui->comboQuote->count()-1)
        return ui->editCustomQuote->text().length() ? ui->editCustomQuote->text().at(0).toLatin1() : 0;

    if(ui->comboQuote->currentText().length())
        return ui->comboQuote->currentText().at(0).toLatin1();
    else
        return 0;
}

void ImportCsvDialog::setSeparatorChar(const QChar& c)
{
    QComboBox* combo = ui->comboSeparator;
    QString sText = c == '\t' ? QString("Tab") : QString(c);
    int index = combo->findText(sText);
    if(index == -1)
    {
        combo->setCurrentIndex(combo->count());
        ui->editCustomSeparator->setText(c);
    }
    else
    {
        combo->setCurrentIndex(index);
    }
}

char ImportCsvDialog::currentSeparatorChar() const
{
    // The last item in the combobox is the 'Other' item; if it is selected return the text of the line edit field instead
    if(ui->comboSeparator->currentIndex() == ui->comboSeparator->count()-1)
        return ui->editCustomSeparator->text().length() ? ui->editCustomSeparator->text().at(0).toLatin1() : 0;

    return ui->comboSeparator->currentText() == tr("Tab") ? '\t' : ui->comboSeparator->currentText().at(0).toLatin1();
}

void ImportCsvDialog::setEncoding(const QString& sEnc)
{
    QComboBox* combo = ui->comboEncoding;
    int index = combo->findText(sEnc);
    if(index == -1)
    {
        combo->setCurrentIndex(combo->count());
        ui->editCustomEncoding->setText(sEnc);
    }
    else
    {
        combo->setCurrentIndex(index);
    }
}

QString ImportCsvDialog::currentEncoding() const
{
    // The last item in the combobox is the 'Other' item; if it is selected return the text of the line edit field instead
    if(ui->comboEncoding->currentIndex() == ui->comboEncoding->count()-1)
        return ui->editCustomEncoding->text().length() ? ui->editCustomEncoding->text() : "UTF-8";
    else
        return ui->comboEncoding->currentText();
}
