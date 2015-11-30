#include <algorithm>
#include <cmath>

#include <berryISelectionService.h>
#include <berryIWorkbenchWindow.h>
#include <QMessageBox>
#include <QShortcut>
#include <mitkImage.h>

#include "MaterialMappingView.h"
#include "lib/WorkbenchUtils/WorkbenchUtils.h"
#include "GuiHelpers.h"
#include "MaterialMappingFilter.h"

const std::string MaterialMappingView::VIEW_ID = "org.mitk.views.materialmapping";

void MaterialMappingView::CreateQtPartControl(QWidget *parent) {
    m_Controls.setupUi(parent);
    // table
    auto table = m_Controls.calibrationTableView;
    table->setModel(m_CalibrationDataModel.getQItemModel());
    auto setResizeMode = [=](int _column, QHeaderView::ResizeMode _mode){
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0) // renamed in 5.0
        table->horizontalHeader()->setResizeMode(_column, _mode);
#else
        table->horizontalHeader()->setSectionResizeMode(_column, _mode);
#endif
    };
    setResizeMode(0, QHeaderView::Stretch);
    setResizeMode(1, QHeaderView::Stretch);

    // data selectors
    m_Controls.unstructuredGridComboBox->SetDataStorage(this->GetDataStorage());
    m_Controls.unstructuredGridComboBox->SetAutoSelectNewItems(false);
    m_Controls.unstructuredGridComboBox->SetPredicate(WorkbenchUtils::createIsUnstructuredGridTypePredicate());

    m_Controls.greyscaleImageComboBox->SetDataStorage(this->GetDataStorage());
    m_Controls.greyscaleImageComboBox->SetAutoSelectNewItems(false);
    m_Controls.greyscaleImageComboBox->SetPredicate(WorkbenchUtils::createIsImageTypePredicate());

    // testing
    if(TESTING){
        m_Controls.testingGroup->show();
        m_Controls.expectedResultComboBox->SetDataStorage(this->GetDataStorage());
        m_Controls.expectedResultComboBox->SetAutoSelectNewItems(false);
        m_Controls.expectedResultComboBox->SetPredicate(WorkbenchUtils::createIsUnstructuredGridTypePredicate());

        m_TestRunner = std::unique_ptr<Testing::Runner>();
//        connect( m_Controls.selectLogFileButton, SIGNAL(clicked()), m_TestRunner.get(), SLOT(openLogFileDialog()) );
    } else {
        m_Controls.testingGroup->hide();
    }

    // delete key on table
    QShortcut* shortcut = new QShortcut(QKeySequence(QKeySequence::Delete), table);
    connect(shortcut, SIGNAL(activated()), this, SLOT(deleteSelectedRows()));

    // signals
    connect( m_Controls.loadButton, SIGNAL(clicked()), this, SLOT(loadButtonClicked()) );
    connect( m_Controls.saveButton, SIGNAL(clicked()), this, SLOT(saveButtonClicked()) );
    connect( m_Controls.startButton, SIGNAL(clicked()), this, SLOT(startButtonClicked()) );
    connect( &m_CalibrationDataModel, SIGNAL(dataChanged()), this, SLOT(tableDataChanged()) );
}

void MaterialMappingView::deleteSelectedRows(){
    auto selection = m_Controls.calibrationTableView->selectionModel();
    auto selectedItems = selection->selectedRows();
    std::set<int> rowsToDelete;

    foreach(auto item, selectedItems){
        rowsToDelete.insert(item.row());
    }

    for(std::set<int>::reverse_iterator rit = rowsToDelete.rbegin(); rit != rowsToDelete.rend(); ++rit){
        m_CalibrationDataModel.removeRow(*rit);
    }
}

void MaterialMappingView::loadButtonClicked() {
    m_CalibrationDataModel.openLoadFileDialog();
}

void MaterialMappingView::saveButtonClicked() {
    m_CalibrationDataModel.openSaveFileDialog();
}

void MaterialMappingView::startButtonClicked() {
    MITK_INFO("ch.zhaw.materialmapping") << "processing input";
    if(isValidSelection()){
        mitk::DataNode *imageNode = m_Controls.greyscaleImageComboBox->GetSelectedNode();
        mitk::DataNode *ugridNode = m_Controls.unstructuredGridComboBox->GetSelectedNode();

        mitk::Image::Pointer image = dynamic_cast<mitk::Image *>(imageNode->GetData());
        mitk::UnstructuredGrid::Pointer ugrid = dynamic_cast<mitk::UnstructuredGrid *>(ugridNode->GetData());

        auto filter = MaterialMappingFilter::New();
        filter->SetInput(ugrid);
        filter->SetIntensityImage(image);
        // TODO:
        auto densityFunctor = createDensityFunctorFromGui();

        filter->SetLinearFunctor([](double _ct){
            return (_ct/1000.0+0.09) / 1.14; // TODO: get from GUI
        });
        filter->SetPowerLawFunctor([](double _ctash){
            return 6850*pow((std::max(_ctash, 0.0)/0.6), 1.49); // TODO: get from GUI
        });

        auto result = filter->GetOutput();
        filter->Update();

        mitk::DataNode::Pointer newNode = mitk::DataNode::New();
        newNode->SetData(result);

        // set some node properties
        newNode->SetProperty("name", mitk::StringProperty::New("material mapped mesh"));
        newNode->SetProperty("layer", mitk::IntProperty::New(1));

        // add result to the storage
        this->GetDataStorage()->Add( newNode );

        if(TESTING){
            if(m_Controls.testingDoComparisonCheckBox->isChecked()){
                mitk::DataNode *expectedResultNode = m_Controls.expectedResultComboBox->GetSelectedNode();
                mitk::UnstructuredGrid::Pointer expectedResult = dynamic_cast<mitk::UnstructuredGrid *>(expectedResultNode->GetData());
                m_TestRunner->compareGrids(result, expectedResult);
            }
        }
    }
}

void MaterialMappingView::tableDataChanged() {
    auto linearEqParams = m_CalibrationDataModel.getFittedLine();
    m_Controls.linEQSlopeSpinBox->setValue(linearEqParams.slope);
    m_Controls.linEQOffsetSpinBox->setValue(linearEqParams.offset);
}

bool MaterialMappingView::isValidSelection() {
    // get the nodes selected
    mitk::DataNode *imageNode = m_Controls.greyscaleImageComboBox->GetSelectedNode();
    mitk::DataNode *ugridNode = m_Controls.unstructuredGridComboBox->GetSelectedNode();

    // set the mandatory field based on whether or not the nodes are NULL
    gui::setMandatoryField(m_Controls.greyscaleSelector, (imageNode == nullptr));
    gui::setMandatoryField(m_Controls.meshSelector, (ugridNode == nullptr));

    if(imageNode && ugridNode){
        mitk::Image::Pointer image = dynamic_cast<mitk::Image *>(imageNode->GetData());
        mitk::UnstructuredGrid::Pointer ugrid = dynamic_cast<mitk::UnstructuredGrid *>(ugridNode->GetData());

        if(image && ugrid){
            return true;
        } else{
            QString msg("Invalid data. Select an image and a unstructured grid.");
            QMessageBox::warning ( NULL, "Error", msg);
        }
    }
    MITK_INFO("ch.zhaw.materialmapping") << "invalid data selection";
    return false;
}

BoneDensityFunctor MaterialMappingView::createDensityFunctorFromGui() {
    BoneDensityFunctor ret;
    ret.SetRhoCt(m_CalibrationDataModel.getFittedLine());

    if(m_Controls.rhoAshCheckBox->isChecked()){
        auto rhoAsh_offset = m_Controls.rhoAshOffsetSpinBox->value();
        auto rhoAsh_divisor = m_Controls.rhoAshDivisorSpinBox->value();
        BoneDensityParameters::RhoAsh rhoAsh(rhoAsh_offset, rhoAsh_divisor);
        ret.SetRhoAsh(rhoAsh);

        if(m_Controls.rhoAppCheckBox->isChecked()){
            auto rhoApp_divisor = m_Controls.rhoAppDivisorSpinBox->value();
            BoneDensityParameters::RhoApp rhoApp(rhoApp_divisor);
            ret.SetRhoApp(rhoApp);
        }
    }
    MITK_INFO("ch.zhaw.materialmapping") << ret;
    return ret;
}
